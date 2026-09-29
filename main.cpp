#include "M_http/M_http.h"
#include "M_timer/M_timer.h"
#include "M_threadpoor/M_threadpool.h"
#include "M_sql_pool/M_sql_pool.h"
#include "M_log/M_log.h"
#include "M_epoll_mode.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstdlib>      // getenv
#include <chrono>


const int   PORT         = 9006;          // 监听端口
const char* DB_HOST      = "localhost";   // 数据库地址
const char* DB_USER      = "root";        // 数据库用户名
const char* DB_NAME      = "webserver";   // 数据库名
const int   DB_PORT      = 3306;          // 数据库端口
const int   SQL_MAX_CONN = 8;             // 连接池最大连接数

const int MAX_FD      = 65536;    // 覆盖 fd 范围（内存吃紧可改小，比如 10000）
const int MAX_EVENTS  = 10000;    // 一次 epoll_wait 最多返回的事件数
const int TIMESLOT_MS = 5000;     // 连接空闲 5 秒就断开

// epoll 触发模式（LT / ET）的开关在 M_epoll_mode.h 里，默认 ET

// 每个连接 = 一个 M_http + 一个 used 标志（timer 指针在 M_http 里）
struct Client {
    M_http http;
    bool used = false;
};

void set_nonblocking(int fd) {
    int old = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old | O_NONBLOCK);
}

// 注册读事件；oneshot 和 M_http::modfd 里保持一致
void addfd(int epollfd, int fd, bool oneshot) {
    epoll_event ev;
    ev.data.fd = fd;
    // EPOLL_TRIGGER = EPOLLET（ET 模式）或 0（LT 模式）
    ev.events  = EPOLLIN | EPOLLRDHUP | EPOLL_TRIGGER;
    if (oneshot) ev.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
}

int main() {
    // 向已关闭的 socket 写数据会收到 SIGPIPE，默认会终止进程，必须忽略
    signal(SIGPIPE, SIG_IGN);

    // 1. 日志（最后一个参数 0 = 同步写，>0 = 异步写）
    M_log::get_instance()->init("./log/server.log", 8192, 5000000, 0);
    LOG_INFO("server 启动");

    // 数据库密码从环境变量读取，不写进代码和版本库：
    //   export WS_DB_PASSWD='your_password'
    const char* db_passwd = getenv("WS_DB_PASSWD");
    if (db_passwd == nullptr) {
        db_passwd = "";
        LOG_WARN("未设置环境变量 WS_DB_PASSWD，数据库密码为空");
    }

    // 2. 数据库连接池（连不上也能服务静态文件，登录/注册不可用而已）
    if (!M_sql_pool::getInstance()->init(DB_HOST, DB_USER, db_passwd,
                                         DB_NAME, DB_PORT, SQL_MAX_CONN)) {
        LOG_WARN("数据库连接池初始化失败，登录/注册不可用，静态文件仍可访问");
    }

    // 3. epoll + M_http 的静态配置
    int epollfd = epoll_create(1);
    if (epollfd < 0) {
        LOG_ERROR("epoll_create 失败");
        return -1;
    }
    M_http::m_epollfd  = epollfd;
    M_http::m_doc_root = "./www";

    // 4. 连接对象数组：fd 当下标，程序启动一次性分配
    Client* users = new Client[MAX_FD];

    // 5. 定时器 + 线程池
    M_timer timer;
    M_threadpool pool(8);   // 业务处理线程，取 CPU 核心数左右即可

    // 6. 监听 socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);
    if (bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind 失败");
        return -1;
    }
    listen(listenfd, 128);
    set_nonblocking(listenfd);
    addfd(epollfd, listenfd, false);   // listenfd 不设 oneshot
    LOG_INFO("监听端口 %d", PORT);

    // 7. timerfd 注册进 epoll（ET 模式，见 M_timer.h 的 fd() 注释）
    epoll_event tev;
    tev.data.fd = timer.fd();
    tev.events  = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, timer.fd(), &tev);

    // 8. 主循环
    epoll_event events[MAX_EVENTS];
    while (true) {
        int n = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listenfd) {
                // 新连接：循环 accept 直到 EAGAIN。
                // ET 下这是必须的——ET 只在“队列由空变非空”时给一次边沿，
                // 如果只 accept 一个就走，队列里剩下的连接可能长期没人处理。
                while (true) {
                    sockaddr_in cli{};
                    socklen_t len = sizeof(cli);
                    int connfd = accept(listenfd, (sockaddr*)&cli, &len);
                    if (connfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        LOG_ERROR("accept 失败");
                        break;
                    }
                    if (connfd >= MAX_FD) {
                        close(connfd);   // fd 超出数组范围，直接拒绝
                        continue;
                    }

                    Client& c = users[connfd];
                    c.used = true;
                    c.http.init(connfd, cli);
                    set_nonblocking(connfd);
                    addfd(epollfd, connfd, true);   // oneshot

                    // 挂定时器：空闲超时回调里只关连接（不能 del 自己，见 M_timer::tick）
                    c.http.timer = timer.add(std::chrono::milliseconds(TIMESLOT_MS),
                        [&c]() { c.http.close_conn(); c.used = false; });
                }
            }
            else if (fd == timer.fd()) {
                timer.handle_read();   // 读掉到期计数 + tick 执行超时回调
            }
            else if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                Client& c = users[fd];
                if (!c.used) continue;   // 已被超时关闭的残留事件
                if (c.http.read()) {
                    // 有活动 -> 刷新超时；业务处理丢给线程池，主线程不等结果
                    timer.adjust(c.http.timer, std::chrono::milliseconds(TIMESLOT_MS));
                    pool.enqueue([&c]() { c.http.process(); });
                } else {
                    timer.del(c.http.timer);   // 读失败 -> 删定时器 + 关连接
                    c.http.close_conn();
                    c.used = false;
                }
            }
            else if (events[i].events & EPOLLOUT) {
                Client& c = users[fd];
                if (!c.used) continue;
                if (!c.http.write()) {         
                    timer.del(c.http.timer);
                    c.http.close_conn();
                    c.used = false;
                } else {
                    timer.adjust(c.http.timer, std::chrono::milliseconds(TIMESLOT_MS));
                }
            }
        }
    }

    close(listenfd);
    close(epollfd);
    delete[] users;
    pool.stop();
    M_sql_pool::getInstance()->closePool();
    return 0;
}
