#include "M_http.h"
#include "../M_sql_pool/M_sql_pool.h"
#include "../M_epoll_mode.h"

#include <sys/epoll.h>    // epoll_event, epoll_ctl
#include <sys/mman.h>     // mmap, munmap
#include <sys/socket.h>   // recv
#include <unistd.h>       // close
#include <fcntl.h>        // open
#include <strings.h>      // strcasecmp, strncasecmp
#include <cstring>        // strpbrk, strspn, strstr, strncpy
#include <cstdio>         // snprintf, vsnprintf
#include <cstdarg>        // va_list
#include <cstdlib>        // atoi
#include <cerrno>         // errno, EAGAIN

// 静态成员定义（server 启动时赋值）
int         M_http::m_epollfd   = -1;
std::string M_http::m_doc_root  = "./www";

// ---------------- 生命周期 ----------------

void M_http::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd  = sockfd;
    m_address = addr;   // 留存对端地址，后续可打印日志
    reset();
}

void M_http::close_conn() {
    unmap();
    if (m_sockfd != -1) {
        close(m_sockfd);
        m_sockfd = -1;
    }
}

// keep-alive：发完一个响应后，回到“读请求行”状态等下一个请求
void M_http::reset() {
    m_read_idx    = 0;
    m_checked_idx = 0;
    m_start_line  = 0;
    m_write_idx   = 0;

    m_check_state   = CHECK_REQUESTLINE;
    m_method        = GET;
    m_url           = nullptr;
    m_version       = nullptr;
    m_host          = nullptr;
    m_content_length = 0;
    m_linger        = false;

    m_body.clear();
    m_resp_body.clear();

    unmap();
    m_iv_count         = 0;
    m_bytes_to_send    = 0;
    m_bytes_have_send  = 0;
}

// ---------------- I/O ----------------

// 非阻塞读：把内核缓冲能拿到的数据一次性读进来，EAGAIN 说明读完了
bool M_http::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;   // 缓冲已满，请求过大，放弃
    }
    while (true) {
        int n = (int)recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;   // 暂时没有更多数据，本轮读结束
            }
            return false;   // 读错误
        }
        if (n == 0) {
            return false;   // 对端关闭连接
        }
        m_read_idx += n;
    }
    return true;
}

// 非阻塞写：writev 一次发“头部缓冲 + mmap 文件”两个 iovec
// 返回 true  = 连接继续（keep-alive 或还没写完）；false = 通知 server 关闭
bool M_http::write() {
    if (m_bytes_to_send == 0) {
        return false;   // 没有要发的（异常情况，直接关）
    }

    while (true) {
        int n = (int)writev(m_sockfd, m_iv, m_iv_count);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                modfd(EPOLLOUT);   // 没写完，重新注册写事件继续
                return true;
            }
            unmap();
            return false;   // 写错误，关闭
        }

        m_bytes_have_send += n;
        m_bytes_to_send   -= n;

        // 从已发送的字节里，跳过已经完全发完的 iovec
        while (m_iv_count > 0 && n >= (int)m_iv[0].iov_len) {
            n -= (int)m_iv[0].iov_len;
            for (int i = 0; i < m_iv_count - 1; ++i) {
                m_iv[i] = m_iv[i + 1];
            }
            --m_iv_count;
        }
        // 若还剩字节，说明当前 iovec 只发出去一部分，调整它的起止
        if (m_iv_count > 0 && n > 0) {
            m_iv[0].iov_base = (char*)m_iv[0].iov_base + n;
            m_iv[0].iov_len  -= n;
        }

        if (m_bytes_to_send <= 0) {
            unmap();
            if (m_linger) {
                reset();          // keep-alive：重置后等下一个请求
                modfd(EPOLLIN);
                return true;
            }
            return false;   // 一次性连接：发完就关
        }
    }
}

// ---------------- 处理主流程（线程池调用） ----------------

void M_http::process() {
    HTTP_CODE ret = process_read();
    if (ret == NO_REQUEST) {
        modfd(EPOLLIN);   // 请求还没读完，重新注册读事件等剩余数据
        return;
    }
    if (!process_write(ret)) {
        close_conn();     // mmap 失败等，直接关
        return;
    }
    modfd(EPOLLOUT);      // 响应就绪，切换监听写事件
}

// 读状态机：请求行 -> 头部 -> 正文
M_http::HTTP_CODE M_http::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE   ret = NO_REQUEST;

    // CHECK_CONTENT 是二进制的正文，不走按行解析；其余状态逐行解析
    while ((m_check_state == CHECK_CONTENT && line_status == LINE_OK)
           || ((line_status = parse_line()) == LINE_OK)) {
        char* text = m_read_buf + m_start_line;   // 当前行/正文起点
        m_start_line = m_checked_idx;             // 下一行从这开始

        switch (m_check_state) {
        case CHECK_REQUESTLINE:
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            break;

        case CHECK_HEADER:
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)      return BAD_REQUEST;
            if (ret == GET_REQUEST)      return do_request();   // 头部结束且无正文
            break;

        case CHECK_CONTENT:
            ret = parse_content(text);
            if (ret == GET_REQUEST)      return do_request();
            line_status = LINE_OPEN;   // 正文没读全，跳出 while 等下一次 read()
            break;

        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// ---------------- 解析 ----------------

// 从 m_read_buf 取一行（以 \r\n 结尾），把它换成 \0\0 便于当作 C 字符串用
M_http::LINE_STATUS M_http::parse_line() {
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        char c = m_read_buf[m_checked_idx];
        if (c == '\r') {
            if (m_checked_idx + 1 == m_read_idx) {
                return LINE_OPEN;   // \r 是最后一个字节，\n 可能还没到
            }
            if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx]     = '\0';
                m_read_buf[m_checked_idx + 1] = '\0';
                m_checked_idx += 2;
                return LINE_OK;
            }
            return LINE_BAD;
        }
        if (c == '\n') {
            // 宽容处理：单独一个 \n 也当行结束（上面的 \r 分支已处理标准 \r\n）
            m_read_buf[m_checked_idx] = '\0';
            ++m_checked_idx;
            return LINE_OK;
        }
    }
    return LINE_OPEN;
}

// 请求行："GET /index.html HTTP/1.1"
M_http::HTTP_CODE M_http::parse_request_line(char* text) {
    m_url = strpbrk(text, " \t");   // 第一个空格处是 method 结束
    if (m_url == nullptr) return BAD_REQUEST;
    *m_url++ = '\0';

    if (strcasecmp(text, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(text, "POST") == 0) {
        m_method = POST;
    } else {
        return BAD_REQUEST;   // 只支持 GET / POST
    }

    m_url += strspn(m_url, " \t");   // 跳过空格，m_url 指向 URL
    m_version = strpbrk(m_url, " \t");
    if (m_version == nullptr) return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    if (strncasecmp(m_version, "HTTP/1.1", 8) != 0) {
        return BAD_REQUEST;   // 只支持 HTTP/1.1（简化）
    }

    m_linger = true;   // HTTP/1.1 默认 keep-alive，Connection: close 会在头部里改
    m_check_state = CHECK_HEADER;
    return NO_REQUEST;
}

// 头部行，遇到空行表示头部结束
M_http::HTTP_CODE M_http::parse_headers(char* text) {
    if (text[0] == '\0') {
        // 空行：头部结束
        if (m_content_length != 0) {
            m_check_state = CHECK_CONTENT;   // 还有正文要读
            return NO_REQUEST;
        }
        return GET_REQUEST;   // 无正文，请求已完整
    }

    if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        m_linger = (strcasecmp(text, "keep-alive") == 0);
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atoi(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    // 其它头部（User-Agent 等）一律忽略

    return NO_REQUEST;
}

// 正文：等 m_read_buf 里凑够 Content-Length 个字节
M_http::HTTP_CODE M_http::parse_content(char* text) {
    if (m_read_idx >= m_content_length + m_checked_idx) {
        text[m_content_length] = '\0';
        m_body = text;   // 保存 POST 正文，供 do_request 解析
        return GET_REQUEST;
    }
    return NO_REQUEST;   // 正文还没读全
}

// ---------------- 决定响应 ----------------

// 根据扩展名返回 Content-Type（学习用途，只覆盖常见几种）
static const char* content_type(const char* path) {
    if (strstr(path, ".html") || strstr(path, ".htm")) return "text/html";
    if (strstr(path, ".css"))   return "text/css";
    if (strstr(path, ".js"))    return "application/javascript";
    if (strstr(path, ".png"))   return "image/png";
    if (strstr(path, ".jpg") || strstr(path, ".jpeg")) return "image/jpeg";
    if (strstr(path, ".ico"))   return "image/x-icon";
    return "text/plain";
}

M_http::HTTP_CODE M_http::do_request() {
    // ---- POST：登录 / 注册 ----
    if (m_method == POST) {
        // 从 m_body 解析 username=xxx&password=yyy
        auto field = [](const std::string& body, const std::string& key) -> std::string {
            size_t pos = body.find(key);
            if (pos == std::string::npos) return "";
            pos += key.size();
            size_t end = body.find('&', pos);
            if (end == std::string::npos) end = body.size();
            return body.substr(pos, end - pos);
        };
        std::string name = field(m_body, "username=");
        std::string pwd  = field(m_body, "password=");

        MYSQL* mysql = nullptr;
        M_sql_poolRAII guard(&mysql, M_sql_pool::getInstance());
        if (mysql == nullptr) {
            m_resp_body = "数据库连接失败";
            return GET_REQUEST;   // GET_REQUEST 表示“正文已就绪”，下同
        }

        // 学习用途：直接字符串拼接 SQL，未做注入防护
        if (m_url != nullptr && strstr(m_url, "register") != nullptr) {
            // 注册：先查重名，再插入
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "SELECT username FROM user WHERE username='%s'", name.c_str());
            bool exists = false;
            if (mysql_query(mysql, sql) == 0) {
                MYSQL_RES* res = mysql_store_result(mysql);
                exists = (res != nullptr && mysql_num_rows(res) > 0);
                if (res != nullptr) mysql_free_result(res);
            }
            if (exists) {
                m_resp_body = "用户名已存在";
            } else {
                snprintf(sql, sizeof(sql),
                         "INSERT INTO user(username, passwd) VALUES('%s','%s')",
                         name.c_str(), pwd.c_str());
                m_resp_body = (mysql_query(mysql, sql) == 0) ? "注册成功" : "注册失败";
            }
        } else if (m_url != nullptr && strstr(m_url, "login") != nullptr) {
            // 登录：查用户名密码是否匹配
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "SELECT username FROM user WHERE username='%s' AND passwd='%s'",
                     name.c_str(), pwd.c_str());
            bool ok = false;
            if (mysql_query(mysql, sql) == 0) {
                MYSQL_RES* res = mysql_store_result(mysql);
                ok = (res != nullptr && mysql_num_rows(res) > 0);
                if (res != nullptr) mysql_free_result(res);
            }
            m_resp_body = ok ? "登录成功" : "用户名或密码错误";
        } else {
            m_resp_body = "未知操作";
        }
        return GET_REQUEST;
    }

    // ---- GET：静态文件 ----
    std::string url = (m_url != nullptr) ? m_url : "/";
    size_t q = url.find('?');   // 去掉查询串 ?a=b
    if (q != std::string::npos) url = url.substr(0, q);

    std::string path = m_doc_root + url;
    if (path.empty() || path.back() == '/') {
        path += "index.html";   // 目录默认返回 index.html
    }

    if (stat(path.c_str(), &m_file_stat) < 0) {
        return NO_RESOURCE;   // 文件不存在
    }
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;   // 简化：不列目录
    }
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;   // 无读权限
    }

    strncpy(m_real_file, path.c_str(), sizeof(m_real_file) - 1);
    m_real_file[sizeof(m_real_file) - 1] = '\0';
    return FILE_REQUEST;
}

// ---------------- 生成响应 ----------------

bool M_http::process_write(HTTP_CODE ret) {
    switch (ret) {
    case INTERNAL_ERROR:
        add_status_line(500, "Internal Error");
        add_content_length(0);
        add_linger();
        add_blank_line();
        break;
    case BAD_REQUEST:
        add_status_line(400, "Bad Request");
        add_content_length(0);
        add_linger();
        add_blank_line();
        break;
    case NO_RESOURCE:
        add_status_line(404, "Not Found");
        add_content_length(0);
        add_linger();
        add_blank_line();
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, "Forbidden");
        add_content_length(0);
        add_linger();
        add_blank_line();
        break;

    case FILE_REQUEST: {
        add_status_line(200, "OK");
        add_response("Content-Type: %s\r\n", content_type(m_real_file));
        add_content_length((int)m_file_stat.st_size);
        add_linger();
        add_blank_line();

        // mmap 映射文件，只读即可
        int fd = open(m_real_file, O_RDONLY);
        if (fd < 0) {
            m_file_address = (char*)MAP_FAILED;
        } else {
            m_file_address = (char*)mmap(nullptr, (size_t)m_file_stat.st_size,
                                         PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
        }
        if (m_file_address == MAP_FAILED) {
            return false;   // 映射失败，上层会 close_conn
        }

        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len  = (size_t)m_write_idx;
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len  = (size_t)m_file_stat.st_size;
        m_iv_count       = 2;
        m_bytes_to_send  = m_write_idx + (int)m_file_stat.st_size;
        break;
    }

    default: {   // GET_REQUEST：登录/注册结果
        add_status_line(200, "OK");
        add_response("Content-Type: text/html\r\n");
        add_content_length((int)m_resp_body.size());
        add_linger();
        add_blank_line();
        add_content(m_resp_body.c_str());

        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len  = (size_t)m_write_idx;
        m_iv_count       = 1;
        m_bytes_to_send  = m_write_idx;
        break;
    }
    }

    // 错误响应（非文件、非登录结果）在上面没设 iovec，这里统一：只发 write_buf
    if (m_iv_count == 0) {
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len  = (size_t)m_write_idx;
        m_iv_count       = 1;
        m_bytes_to_send  = m_write_idx;
    }

    m_bytes_have_send = 0;
    return true;
}

// ---------------- 拼响应的辅助函数 ----------------

bool M_http::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) return false;

    va_list args;
    va_start(args, format);
    int len = vsnprintf(m_write_buf + m_write_idx,
                        WRITE_BUFFER_SIZE - m_write_idx, format, args);
    va_end(args);

    if (len < 0 || m_write_idx + len >= WRITE_BUFFER_SIZE) return false;
    m_write_idx += len;
    return true;
}

bool M_http::add_content(const char* content) {
    return add_response("%s", content);
}

bool M_http::add_status_line(int status, const char* title) {
    return add_response("HTTP/1.1 %d %s\r\n", status, title);
}

bool M_http::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}

bool M_http::add_linger() {
    return add_response("Connection: %s\r\n", m_linger ? "keep-alive" : "close");
}

bool M_http::add_blank_line() {
    return add_response("\r\n");
}

// ---------------- mmap / epoll ----------------

void M_http::unmap() {
    if (m_file_address != nullptr && m_file_address != MAP_FAILED) {
        munmap(m_file_address, (size_t)m_file_stat.st_size);
        m_file_address = nullptr;
    }
}

// 重新注册本连接的监听事件（读事件/写事件），用 EPOLLONESHOT 防止多线程抢同一个连接
//
// EPOLL_CTL_MOD 会重新检查一次 fd 的就绪状态，这正是 ET + ONESHOT 能安全工作的地方：
// 即使 read() 读到 EAGAIN 之后、这里重新注册之前对端又发来了数据，MOD 也会把
// 这个依旧可读的 fd 重新挂回就绪队列，不会漏掉。
void M_http::modfd(int ev) {
    struct epoll_event event;
    event.data.fd = m_sockfd;
    event.events  = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLL_TRIGGER;
    epoll_ctl(m_epollfd, EPOLL_CTL_MOD, m_sockfd, &event);
}
