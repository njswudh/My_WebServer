# My WebServer

一个用 C++17 从零实现的 Linux 高并发 Web 服务器，主要目的是学习网络编程：
`epoll(ET) + EPOLLONESHOT` 事件驱动、线程池处理业务、HTTP/1.1 解析状态机、
`timerfd` 连接超时管理，以及 MySQL(MariaDB) 连接池。

## 特性

- **事件驱动**：epoll 边沿触发（ET）+ `EPOLLONESHOT`，每个连接同一时刻只被一个线程处理
- **I/O 与业务分离**：主线程只做 `read`/`write`，解析与响应生成丢给线程池
- **HTTP/1.1 状态机解析**：请求行 → 头部 → 正文，可重入，正确应对 TCP 粘包/半包
- **keep-alive**：默认长连接，按 `Connection` 头决定复用还是关闭
- **静态文件服务**：`mmap` 映射文件 + `writev` 一次系统调用发出「响应头 + 文件」
- **连接超时**：最小堆定时器 + `timerfd`，空闲超时自动断开
- **日志**：支持同步/异步写，按天分文件
- **数据库连接池**：启动时预建连接，RAII 自动归还
- **示例业务**：`/login`、`/register` 两个 POST 接口

## 构建

依赖：

- Linux（用到 epoll / timerfd / mmap）
- g++，支持 C++17
- MariaDB 客户端开发库（头文件 `<mariadb/mysql.h>`，链接 `-lmariadb`）

Debian/Ubuntu 安装依赖：

```bash
sudo apt install build-essential libmariadb-dev
```

编译：

```bash
make          # 生成可执行文件 server
make clean    # 删除 .o 与 server
```

## 运行

数据库密码不再写死在代码里，而是启动时从环境变量 `WS_DB_PASSWD` 读取：

```bash
export WS_DB_PASSWD='你的数据库密码'
./server
```

或者只在本次启动时提供：

```bash
WS_DB_PASSWD='你的数据库密码' ./server
```

没设置也不会崩，只是登录/注册用不了，静态文件照常服务，日志里会记一条提醒。

启动后浏览器访问 <http://localhost:9006/>，登录/注册测试页在
<http://localhost:9006/login.html>。

## 配置

目前配置是 `main.cpp` 顶部的编译期常量：

| 常量 | 默认值 | 说明 |
| --- | --- | --- |
| `PORT` | `9006` | HTTP 监听端口 |
| `DB_HOST` | `localhost` | 数据库地址 |
| `DB_USER` | `root` | 数据库用户名 |
| `DB_NAME` | `webserver` | 数据库名 |
| `DB_PORT` | `3306` | 数据库端口 |
| `SQL_MAX_CONN` | `8` | 连接池最大连接数 |
| `TIMESLOT_MS` | `5000` | 连接空闲超时（毫秒） |
| `MAX_FD` | `65536` | 连接对象数组容量 |
| `MAX_EVENTS` | `10000` | 单次 `epoll_wait` 最多返回的事件数 |
| 数据库密码 | 环境变量 `WS_DB_PASSWD` | 不写进代码/版本库 |

静态资源根目录由 `M_http::m_doc_root` 指定，默认 `./www`。

## 数据库准备

登录/注册功能需要一张 `user` 表（本项目不做建表，需自行创建）：

```sql
CREATE DATABASE IF NOT EXISTS webserver DEFAULT CHARSET utf8mb4;
USE webserver;

CREATE TABLE IF NOT EXISTS user (
    username VARCHAR(64) NOT NULL PRIMARY KEY,
    passwd   VARCHAR(64) NOT NULL
);
```

## HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | 任意路径 | 返回 `www` 下的静态文件；以 `/` 结尾时返回 `index.html` |
| POST | `/login` | 正文 `username=...&password=...`，返回「登录成功」或「用户名或密码错误」 |
| POST | `/register` | 正文同上，返回「注册成功」/「用户名已存在」/「注册失败」 |

响应状态码：

| 状态码 | 触发场景 |
| --- | --- |
| 200 OK | 静态文件存在、登录/注册处理完成 |
| 400 Bad Request | 请求行/头部格式错误，或路径指向目录 |
| 403 Forbidden | 文件存在但没有 other 读权限 |
| 404 Not Found | 文件不存在 |
| 500 Internal Error | 解析状态机进入未知状态等内部错误 |

## 架构

```
                   主线程（Reactor）
   ┌────────────────────────────────────────────────┐
   │  epoll_wait                                    │
   │    ├─ listenfd → accept 循环到 EAGAIN           │
   │    ├─ timerfd  → 执行到期回调（关闭空闲连接）    │
   │    ├─ EPOLLIN  → M_http::read()                │
   │    │              └─ 有数据 → 丢给线程池         │
   │    └─ EPOLLOUT → M_http::write()               │
   └────────────────────────────────────────────────┘
                        │ enqueue
                        ▼
   ┌────────────────────────────────────────────────┐
   │  线程池（默认 8 个线程）                         │
   │    M_http::process()                           │
   │      解析请求 → 生成响应 → modfd(EPOLLOUT)      │
   └────────────────────────────────────────────────┘
```

一个 TCP 连接对应一个 `M_http` 对象，读写缓冲、解析游标、响应 iovec 都放在对象里；
`M_http::m_epollfd` 与 `M_http::m_doc_root` 是全局配置。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `main.cpp` | 主循环：epoll 事件分发、accept、定时器与线程池装配、配置常量 |
| `M_epoll_mode.h` | LT / ET 触发模式开关（宏 `ET_EPOLL`，默认 ET） |
| `M_http/` | HTTP 连接对象：状态机解析、响应生成、`writev` 发送 |
| `M_timer/` | 最小堆定时器 + `timerfd`，连接空闲超时 |
| `M_threadpoor/` | 线程池（`enqueue` 提交任务，返回 `std::future`） |
| `M_sql_pool/` | MariaDB 连接池与 RAII 取用类 |
| `M_log/` | 日志组件，支持同步/异步、按天分文件 |
| `www/` | 静态资源：`index.html`、`login.html` |
| `log/` | 运行日志输出目录（日志内容不进版本库） |

## 解析流程

`M_http::process_read()` 是一个状态机，状态定义在 `M_http.h`：

| 状态 | 含义 |
| --- | --- |
| `CHECK_REQUESTLINE` | 正在读请求行 `METHOD URL VERSION` |
| `CHECK_HEADER` | 正在读头部，直到空行为止 |
| `CHECK_CONTENT` | 正在按 `Content-Length` 收正文（不按行解析） |

返回码 `HTTP_CODE` 同时承担「解析进度」和「该回什么响应」两种语义：

| 返回码 | 含义 |
| --- | --- |
| `NO_REQUEST` | 数据还不完整，重新注册读事件继续等 |
| `GET_REQUEST` | 请求完整，响应体已在 `m_resp_body` |
| `FILE_REQUEST` | 请求完整，需要发送 `m_real_file` |
| `NO_RESOURCE` | 文件不存在 → 404 |
| `FORBIDDEN_REQUEST` | 没有读权限 → 403 |
| `BAD_REQUEST` | 报文格式错误 → 400 |
| `INTERNAL_ERROR` | 内部错误 → 500 |

解析用三个游标在同一个读缓冲上推进，不额外分配字符串：

- `m_read_idx`：已读入的字节数
- `m_checked_idx`：已经扫描到的位置
- `m_start_line`：当前行的起点

`parse_line()` 把行尾的 `\r\n` 就地改写为 `\0\0`，这样每一行都是现成的 C 字符串，
`m_url` / `m_version` / `m_host` 都是指向读缓冲内部的指针（零拷贝，但只在本次解析内有效）。

## ET 模式的三个硬性要求

ET 只在 不可读 → 可读 的边沿通知一次，所以：

1. `accept` 必须循环到 `EAGAIN`，否则连接队列里的剩余连接等不到下一次通知；
2. `read()` 必须循环到 `EAGAIN`，否则内核缓冲里的剩余数据不会再触发事件；
3. 所有 fd 注册（`addfd` 与 `M_http::modfd`）都要带 `EPOLLET`。

三者通过 `M_epoll_mode.h` 的 `EPOLL_TRIGGER` 宏统一，避免只改一处导致连接卡死。
想切回水平触发，注释掉该文件里的 `#define ET_EPOLL` 即可。

## 已知限制


- 只支持 `GET` / `POST` 和 HTTP/1.1，不处理 `HEAD`、`PUT` 等
- 不支持请求流水线（pipelining）、分块传输（`Transfer-Encoding: chunked`）、压缩
- 读缓冲固定 2048 字节、写缓冲固定 1024 字节，超长请求会被直接断开
- SQL 用字符串拼接，**未做注入防护**，仅用于学习
- 静态文件路径未做规范化，理论上存在 `../` 路径穿越风险
- 目录请求直接返回 400，不提供目录列表

