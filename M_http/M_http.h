#pragma once
#include <netinet/in.h>   // sockaddr_in
#include <sys/stat.h>     // struct stat
#include <sys/uio.h>      // struct iovec
#include <string>

struct M_timer_node;      // 前向声明即可，成员只是指针

// HTTP 连接处理类：读请求 -> 解析 -> 生成响应 -> 写响应。
// 线程模型（Proactor）：read()/write() 由主线程调用，process() 由线程池调用。
class M_http {
public:
    static const int READ_BUFFER_SIZE  = 2048;   // 读缓冲
    static const int WRITE_BUFFER_SIZE = 1024;   // 写缓冲

    M_http()  = default;
    ~M_http() = default;

    void init(int sockfd, const sockaddr_in& addr);  // 新连接初始化
    void close_conn();                                // 关闭连接（定时器超时也调它）

    void process();   // 解析 + 生成响应（由线程池调用）
    bool read();      // 非阻塞读（主线程读事件时调用）
    bool write();     // 非阻塞写（主线程写事件时调用）

    static int         m_epollfd;    // epoll 实例，server 启动时设置一次
    static std::string m_doc_root;   // 网站根目录，server 启动时设置一次
    M_timer_node* timer = nullptr;   // 定时器节点（server 设置/调整），public 简单直观

private:
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    enum CHECK_STATE { CHECK_REQUESTLINE = 0, CHECK_HEADER, CHECK_CONTENT };
    enum HTTP_CODE   { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
                       FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR };
    enum METHOD      { GET = 0, POST };

    void reset();                       // keep-alive：重置回“读请求行”状态
    HTTP_CODE process_read();           // 读状态机
    bool process_write(HTTP_CODE ret);  // 生成响应
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();             // 决定响应：静态文件 or 登录/注册
    LINE_STATUS parse_line();           // 从读缓冲取一行（\r\n 结尾）
    void unmap();                       // 解除 mmap
    void modfd(int ev);                 // 重新注册本连接的监听事件

    // 拼响应
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

private:
    int m_sockfd = -1;
    sockaddr_in m_address{};

    // 读缓冲 + 三个游标
    char m_read_buf[READ_BUFFER_SIZE]{};
    int m_read_idx    = 0;   // 已读入字节数
    int m_checked_idx = 0;   // 已解析到的位置
    int m_start_line  = 0;   // 当前行起点

    // 写缓冲
    char m_write_buf[WRITE_BUFFER_SIZE]{};
    int m_write_idx = 0;

    // 解析状态
    CHECK_STATE m_check_state = CHECK_REQUESTLINE;
    METHOD m_method = GET;

    // 请求信息（url/version/host 都指向 m_read_buf 内的位置，不额外分配）
    char m_real_file[256]{};   // 请求映射到的真实文件路径
    char* m_url = nullptr;
    char* m_version = nullptr;
    char* m_host = nullptr;
    int m_content_length = 0;
    bool m_linger = false;     // 是否 keep-alive
    std::string m_body;        // POST 正文（username=...&password=...）
    std::string m_resp_body;   // 登录/注册结果的响应正文

    // 响应文件传输
    char* m_file_address = nullptr;   // mmap 地址
    struct stat m_file_stat{};
    struct iovec m_iv[2]{};          // [0]=写缓冲头部, [1]=mmap 文件
    int m_iv_count = 0;
    int m_bytes_to_send = 0;
    int m_bytes_have_send = 0;
};
