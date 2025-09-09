#include "http_conn.h"

// HTTP响应状态相关字符串
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站根目录
const char *doc_root = "/home/asus/linux-high-effective/linux-high-effective/Pool_of_thread_process/code/my_tiny_web/output/www";
// 设置文件描述符为非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; // 采用EPOLLET边缘触发模式
    // 水平触发 LT默认模式 当文件描述符上fd有未处理的事件 epoll会持续发通知 知道事件被完全处理 例如如果有100字节 只读了50字节 epoll会在每次epoll_wait时都返回该fd的可读事件 直到100字节全被读取
    // 边缘触发 仅在fd状态发生变化的时触发一次通知 如果只读取了50字节 剩余的字节 会在新写入的数据中被读取 可以配合非阻塞的IO 实现一次性处理完所有当前可用的数据
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 初始化静态成员
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

// 关闭连接
void http_conn::close_conn(bool real_close)
{
    if (real_close && m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化新连接
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 允许地址重用
    int reuse = 1;
    // SOL_SOCKET说明 当前选项所属的层级为套接字层
    // SO_REUSEADDR 表示运行重用本地地址和端口
    // 在 TCP 协议中，当一个套接字关闭后，其占用的端口会进入 TIME_WAIT 状态（通常持续几分钟），用于确保网络中残留的数据包被正确处理。若此时尝试用同一个端口重新创建套接字，默认情况下会失败（提示 “地址已在使用中”）。
    // SO_REUSEADDR 选项的作用是允许在端口处于 TIME_WAIT 状态时，重新绑定该端口
    // 在服务器 程序频繁重启和快速恢复的服务十分有用
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 加入epoll
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

// 初始化连接内部状态
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = m_version = m_host = nullptr;
    m_content_length = 0;
    m_start_line = 0;

    m_checked_idx = m_read_idx = m_write_idx = 0;
    m_iv_count = 0;  // 确保初始化m_iv_count
    m_file_address = nullptr; // 确保初始化文件地址
    
    memset(m_read_buf, 0, READ_BUFFER_SIZE);
    memset(m_write_buf, 0, WRITE_BUFFER_SIZE);
    memset(m_real_file, 0, FILENAME_LEN);
}

// 解析一行HTTP数据
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx]; // temp等与每一个缓冲区的字节
        if (temp == '\r')
        {
            if (m_checked_idx + 1 == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 把/r/n当成\0 表面这一行读取完毕
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 非阻塞读数据
bool http_conn::read()
{
    printf("Debug: Starting read, current idx: %d\n", m_read_idx); // 添加调试输出
    if (m_read_idx >= READ_BUFFER_SIZE)                            // 说明读缓冲区以及存满,没有剩余空间再接受新数据
        return false;
    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)                         // 如果有错误的话
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) // 如果没有数据可读的话 或者 是非阻塞的话 直接退出循环
                break;
            return false;
        }
        else if (bytes_read == 0)
            return false;         // 对方关闭连接
        m_read_idx += bytes_read; // 更新位置
    }
    return true;
}

// 解析HTTP请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;

    if (strcasecmp(method, "GET") != 0)
    {
        return BAD_REQUEST;
    }

    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';

    printf("Debug: URL: '%s', Version: '%s'\n", m_url, m_version); // 添加调试输出

    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    return NO_REQUEST;
}

// 解析HTTP请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (*text == '\0')
    { // 空行表示请求头结束
        return m_content_length ? NO_REQUEST : GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) // 返回0 表是请求成功
    {
        text += 11;
        text += strspn(text, " \t"); // 功能是计算字符串text开头连续匹配字符集" \t"（空格或制表符）的长度
        m_linger = (strcasecmp(text, "keep-alive") == 0);
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atoi(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // 忽略其他请求头
    }
    return NO_REQUEST;
}

// 解析HTTP请求体 并没有解析 只是判断是否被完整的读入了
// 仅仅post报文中有 请求体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= m_content_length + m_start_line)
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 处理HTTP请求（解析+业务逻辑）
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = nullptr;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           (line_status = parse_line()) == LINE_OK)
    {
        text = get_line();
        m_start_line = m_checked_idx;

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            m_check_state = CHECK_STATE_HEADER;
            break;
        case CHECK_STATE_HEADER:
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
                return do_request();
            break;
        case CHECK_STATE_CONTENT:
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN; // 告知主循环：当前请求体数据尚未完全读取，解析过程需要暂停，等待更多数据到达后再继续。
            break;
        default:
            return INTERNAL_ERROR;
        }
    }

    return line_status == LINE_OPEN ? NO_REQUEST : BAD_REQUEST;
}

// 处理请求（映射目标文件）
http_conn::HTTP_CODE http_conn::do_request()
{
    // 处理URL，移除查询参数（问号后面的部分）
    char url_path[FILENAME_LEN] = {0};
    strcpy(url_path, m_url);
    // 找到第一个?，并在那里截断URL
    char* query_start = strchr(url_path, '?');
    if (query_start) {
        *query_start = '\0';
    }
    
    strcpy(m_real_file, doc_root); // 从doc_root中复制到m_real_life
    int len = strlen(doc_root);
    strncpy(m_real_file + len, url_path, FILENAME_LEN - len - 1);

    struct stat st;
    if (stat(m_real_file, &st) == 0 && S_ISDIR(st.st_mode))
    {
        // 是目录，添加默认文件 index.html
        printf("Path is directory, adding index.html\n");
        
        // 检查路径末尾是否已有斜杠
        int path_len = strlen(m_real_file);
        if (path_len > 0 && m_real_file[path_len-1] != '/') {
            strcat(m_real_file, "/");
        }
        strcat(m_real_file, "index.html");
    }
    
    // stat 是一个系统调用函数，用于获取指定文件（FILE）的属性信息，并将这些信息存储到一个结构体（BUF）中。
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    
    if (!(m_file_stat.st_mode & S_IROTH)) { // S_IROTH是表示其他用户（others）拥有读权限
        return FORBIDDEN_REQUEST;
    }
    
    if (S_ISDIR(m_file_stat.st_mode)) {
        return NO_RESOURCE; // 注意网站没有做 就是首页目录没做好
    }

    int fd = open(m_real_file, O_RDONLY);
    if (fd < 0) {
        return NO_RESOURCE;
    }
    
    /*
    nullptr：让系统自动选择映射的起始内存地址（不手动指定）。
    m_file_stat.st_size：映射的长度（字节数），即目标文件的大小（通过 stat 函数获取）。
    PROT_READ：映射区域的保护模式为只读（进程只能读取该内存区域，不能修改）。
    MAP_PRIVATE：创建私有映射（写时复制，进程对映射区域的修改不会影响原文件，也不会被其他进程看到）。
    fd：要映射的文件的文件描述符（通过 open 函数打开的目标文件）。
    0：映射的偏移量（从文件起始位置开始映射）。
    */
    m_file_address = (char *)mmap(nullptr, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m_file_address == MAP_FAILED) {
        close(fd);
        return NO_RESOURCE;
    }
    
    close(fd);
    return FILE_REQUEST;
}

// 释放内存映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = nullptr;
    }
}

// 向写缓冲区添加响应内容（格式化）
// 这个似乎可以c++泛型化
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;                                                                                      // 函数参数是可变参数 需要用va_list类型的遍历arg_list接受参数列表
    va_start(arg_list, format);                                                                            // va_start用于初始化arg_list 使其指向第一个可变参数 format是最后一个固定参数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arg_list); // 功能是将 format 格式的字符串与可变参数 arg_list 拼接后，写入 m_write_buf + m_write_idx 位置（即缓冲区的剩余空间）
    // WRITE_BUFFER_SIZE - m_write_idx - 1 预留空间 len是返回的实际写入的数量
    va_end(arg_list);
    if (len >= WRITE_BUFFER_SIZE - m_write_idx - 1)
        return false;
    m_write_idx += len;
    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("HTTP/1.1 %d %s\r\n", status, title);
}
// 添加响应头集合
bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
    return true;
}
// 添加内容长度头
bool http_conn::add_content_length(int content_length)
{
    return add_response("Content-Length: %d\r\n", content_length);
}
// 添加连接保持头
bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", m_linger ? "keep-alive" : "close");
}
// 添加空行（HTTP头结束标志）
bool http_conn::add_blank_line()
{
    return add_response("\r\n");
}
// 添加响应内容
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 填充HTTP响应并准备写操作
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        add_content(error_500_form);
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        add_content(error_400_form);
        break;
    case NO_RESOURCE:
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        add_content(error_404_form);
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        add_content(error_403_form);
        break;
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size)
        {
            add_headers(m_file_stat.st_size);
            // 不要在这里直接返回，让下面的代码设置m_iv和m_iv_count
        }
        else // 如果目标文件的大小为0
        {
            const char *empty_html = "<html><body></body></html>";
            add_headers(strlen(empty_html));
            add_content(empty_html);
        }
        break;
    default:
        return false;
    }

    // 为后续的writev做充足的准备
    // writev 系统调用可以通过一次系统调用写入多个不连续的内存块（即 m_iv 数组中的所有块）
    //  组装写缓冲区（仅响应头，无文件内容时,所以只需要一个内存块）
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1; // 至少有一个块（响应头）
    
    if (m_file_address)
    { // 有文件内容时，用writev同时写响应头和文件内存
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
    }
    
    return true;
}

// 非阻塞写数据（用writev批量写）
bool http_conn::write()
{
    int bytes_to_send = 0;
    if (m_iv_count == 0) {
        if (m_write_idx > 0) {
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv_count = 1;
        } else {
            return false;
        }
    }

    for (int i = 0; i < m_iv_count; i++) {
        bytes_to_send += m_iv[i].iov_len;
    }
    
    int bytes_have_send = 0;
    
    while (bytes_to_send > 0)
    {
        int temp = writev(m_sockfd, m_iv, m_iv_count);
        
        if (temp <= -1)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);  // 重新注册写事件
                return true;
            }
            unmap();
            return false;
        }
        
        bytes_have_send += temp;
        bytes_to_send -= temp;
        
        // 更新iovec数组，处理部分写入情况
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            // 响应头已发送完
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = (char*)m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 响应头未发送完
            m_iv[0].iov_base = (char*)m_iv[0].iov_base + bytes_have_send;
            m_iv[0].iov_len -= bytes_have_send;
        }
        
        // 数据已全部发送
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);  // 重新注册读事件
            
            if (m_linger)
            {
                init();  // 重置连接状态，准备下一个请求
                return true;
            }
            else
            {
                return false;  // 返回false会关闭连接
            }
        }
    }
    
    return true;
}

// 处理客户请求的入口（调度读/写）由线程池子中的工作线程调用
void http_conn::process()
{
    sleep(3);
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        // 由于设置了EPOLLONESHOT 该事件只会被处理一次 如果没有处理完毕 程序必须重新设置m_sockfd 确保报文下一次到来的时候能被线程处理
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 重新监听读事件
        return;
    }

    bool write_ret = process_write(read_ret);
    
    if (!write_ret)
        close_conn();

    // 这里首次设置写事件的监听
    modfd(m_epollfd, m_sockfd, EPOLLOUT); // 监听写事件
}