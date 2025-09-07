#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "/home/asus/linux-high-effective/linux-high-effective/multithread-programming/code/locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536           // 最大文件描述符数
#define MAX_EVENT_NUMBER 10000 // epoll最大监听事件数

// 注册信号处理函数
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask); //用于将所有信号加入到信号屏蔽字中，确保在处理当前信号时不会被其他信号打断
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 向客户端发送错误信息并关闭连接
void show_error(int connfd, const char* info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char* argv[]) {
    if (argc <= 2) {
        printf("Usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    // 忽略SIGPIPE信号（避免写关闭的连接导致进程终止）
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池
    threadpool<http_conn>* pool = nullptr;
    try {
        pool = new threadpool<http_conn>;
    } catch (...) {
        return 1;
    }

    // 预分配HTTP连接对象数组
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    // 创建监听socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    // 允许地址重用 方便与服务器多次启动
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定地址
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    assert(bind(listenfd, (struct sockaddr*)&address, sizeof(address)) != -1);
    assert(listen(listenfd, 5) != -1); // 监听队列长度为5
    
    printf("Server started, listening on %s:%d\n", ip, port);

    // 创建epoll实例
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false); // 监听socket不设EPOLLONESHOT
    http_conn::m_epollfd = epollfd;

    while (true) {
        // epoll等待事件
        int event_count = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (event_count < 0 && errno != EINTR) {
            printf("epoll failure\n");
            break;
        }

        // 处理每个事件
        for (int i = 0; i < event_count; ++i) {
            int sockfd = events[i].data.fd;

            // 新连接事件
            if (sockfd == listenfd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
                if (connfd < 0) {
                    printf("accept error: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD) {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_addr); // 初始化新连接
            } 
            // 连接关闭/错误事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) { 
                //EPOLLRDHUP：表示对端关闭了连接（即 TCP 的 FIN 包已到达），常用于检测客户端主动断开连接
                //EPOLLHUP：表示挂起事件，通常是 socket 被关闭或出现严重错误时触发。它意味着连接已经不可用。
                //EPOLLERR：表示发生错误事件，如 socket 出现异常（比如写入/读取错误），需要及时处理。
                users[sockfd].close_conn();
            } 
            // 读事件
            else if (events[i].events & EPOLLIN) {
                if (users[sockfd].read()) { // 读取成功
                    pool->append(users + sockfd); // 加入线程池处理
                } else {
                    users[sockfd].close_conn(); // 读取失败则关闭
                }
            } 
            // 写事件
            else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) { // 写入失败则关闭
                    users[sockfd].close_conn();
                }
            }
        }
    }

    // 资源释放
    close(epollfd);
    close(listenfd);
    delete pool;
    delete[] users;

    return 0;
}