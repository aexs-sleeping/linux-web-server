#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include<sys/uio.h>
#include "/home/asus/linux-high-effective/linux-high-effective/multithread-programming/code/locker.h"


class http_conn
{
public:
    /* 文件名的最大长度 */
    static const int FILENAME_LEN = 200;

    /* 读缓冲区的大小 */
    static const int READ_BUFFER_SIZE = 2048;
    /* 写缓冲区的大小 */
    static const int WRITE_BUFFER_SIZE = 1024;
/* HTTP请求方法，但我们仅支持GET */
enum METHOD { 
    GET = 0,        // 获取资源（代码中主要支持的方法）
    POST,           // 提交数据到服务器（如表单提交）
    HEAD,           // 类似GET，但仅返回响应头，不返回响应体
    PUT,            // 上传资源到服务器（替换目标资源）
    DELETE,         // 请求删除服务器上的指定资源
    TRACE,          // 回显服务器收到的请求，用于测试或诊断
    OPTIONS,        // 查询服务器支持的HTTP方法及其他选项
    CONNECT,        // 建立隧道连接（通常用于HTTPS的SSL/TLS握手）
    PATCH           // 部分更新资源（仅修改资源的部分内容）
};

/* 解析客户请求时，主状态机所处的状态 */
enum CHECK_STATE { 
    CHECK_STATE_REQUESTLINE = 0,  // 初始状态：正在解析HTTP请求行（如"GET /index.html HTTP/1.1"）
    CHECK_STATE_HEADER,           // 请求行解析完成：正在解析HTTP请求头（如"Host: example.com"）
    CHECK_STATE_CONTENT           // 请求头解析完成：若有请求体（如POST数据），则正在解析请求体
};

/* 服务器处理HTTP请求的可能结果 */
enum HTTP_CODE 
{ 
    NO_REQUEST,           // 请求未完全解析（需继续读取数据后重新解析）
    GET_REQUEST,          // 请求完全解析且格式正确，可执行后续业务处理
    BAD_REQUEST,          // 请求格式错误（如请求行、请求头语法错误）
    NO_RESOURCE,          // 请求的资源不存在（如目标文件未找到）
    FORBIDDEN_REQUEST,    // 请求的资源存在，但客户端无访问权限（如文件不可读）
    FILE_REQUEST,         // 请求的资源存在且可访问，已准备好返回文件内容
    INTERNAL_ERROR,       // 服务器内部错误（如代码逻辑异常）
    CLOSED_CONNECTION     // 客户端主动关闭连接
};

/* 行的读取状态（用于判断HTTP请求中单行数据的解析结果） */
enum LINE_STATUS {
    LINE_OK = 0,    // 成功解析一行（符合HTTP格式，以"\r\n"结尾）
    LINE_BAD,       // 行格式错误（如仅含"\r"或"\n"，无成对出现）
    LINE_OPEN       // 行未完全读取（缓冲区数据不足，需继续读取）
};

public:
    http_conn() {}
    ~http_conn() {}

    /* 初始化新接受的连接 */
    void init(int sockfd, const sockaddr_in& addr);
    /* 关闭连接 */
    void close_conn(bool real_close = true);
    /* 处理客户请求 */
    void process();
    /* 非阻塞读操作 */
    bool read();
    /* 非阻塞写操作 */
    bool write();

private:
    /* 初始化连接 */
    void init();
    /* 解析HTTP请求 */
    HTTP_CODE process_read();
    /* 填充HTTP应答 */
    bool process_write(HTTP_CODE ret);

    /* 下面这一组函数被process_read调用以分析HTTP请求 */
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();

    /* 下面这一组函数被process_write调用以填充HTTP应答 */
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    /* 所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的 */
    static int m_epollfd;
    /* 统计用户数量 */
    static int m_user_count;

private:
    /* 该HTTP连接的socket和对方的socket地址 */
    int m_sockfd;
    sockaddr_in m_address;

    /* 读缓冲区 */
    char m_read_buf[READ_BUFFER_SIZE];
    /* 标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置 */
    int m_read_idx;
    /* 当前正在分析的字符在读缓冲区中的位置 */
    int m_checked_idx;
    /* 当前正在解析的行的起始位置 */
    int m_start_line;
    /* 写缓冲区 */
    char m_write_buf[WRITE_BUFFER_SIZE];
    /* 写缓冲区中待发送的字节数 */
    int m_write_idx;

    /* 主状态机当前所处的状态 */
    CHECK_STATE m_check_state;
    /* 请求方法 */
    METHOD m_method;

    /* 客户请求的目标文件的完整路径，其内容等于doc_root + m_url, doc_root是网站根目录 */
    char m_real_file[FILENAME_LEN];
    /* 客户请求的目标文件名 */
    char* m_url;
    /* HTTP协议版本号，我们仅支持HTTP/1.1 */
    char* m_version;
    /* 主机名 */
    char* m_host;
    /* HTTP请求的消息体的长度 */
    int m_content_length;
    /* HTTP请求是否要求保持连接 */
    bool m_linger;

    /* 客户请求的目标文件被mmap到内存中的起始位置 */
    char* m_file_address;
    /* 目标文件的状态。用于判断文件是否存在、是否为目录、是否可读等 */
    struct stat m_file_stat;

    /* 采用writev执行写操作时的内存块相关成员，m_iv_count表示被写内存块的数量 */
    struct iovec m_iv[2];
    //iovec的核心作用是描述一块内存的"起始地址和长度" 配合writev和readv系统调用实现分散读和集中写 从而提高IO效率
    /*
        传统的write和read一次只能操作一个缓冲区 如果要发送/接受多段数据 比如HTTP响应头+响应体 需要多次调用write/read
        每次write/read都需要用户态->内核态的切换 而且多次write无法保证数据的连续写入 可能 被其他进程的写操作打断
        write和readv+iovec一次可以系统同调用多个缓冲区
    */
    int m_iv_count;
};
int setnonblocking(int fd);

/// @brief 将文件描述符添加到epoll
/// @param epollfd 
/// @param fd 
/// @param one_shot epoll下的一种事件触发模式,该文件描述符上的epoll事件只会被epoll触发一次 一旦被处理过 epoll也不会再向事件列表中添加这个事件 知道通过epoll_ctl重新设置事件
void addfd(int epollfd, int fd, bool one_shot);

/// @brief 修改已注册到 epoll 实例中的文件描述符（fd）的监听事件属性
/// @param epollfd epoll内核事件表的标识
/// @param fd 需要修改事件属性的目标文件描述符 通常是客户端的socket
/// @param ev 用户指定的核心事件
void modfd(int epollfd, int fd, int ev);

/// @brief  从epoll中移除文件描述符
/// @param epollfd 
/// @param fd 
void removefd(int epollfd, int fd);
#endif