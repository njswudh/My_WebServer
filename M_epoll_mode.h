#ifndef M_EPOLL_MODE_H
#define M_EPOLL_MODE_H

#include <sys/epoll.h>   // EPOLLET

// epoll 触发模式开关：默认 ET（边沿触发）。
//
// 想切回 LT（水平触发）：注释掉下面这行即可。
//
// 为什么要单独放一个头文件：ET 对代码有硬性要求，而要求分散在两个文件里——
//   1) main.cpp 的 accept 必须循环到 EAGAIN，否则 accept 队列里剩下的连接
//      等不到下一次边沿，会长期积压；
//   2) M_http::read() 必须循环读到 EAGAIN，否则内核里剩下的数据不会再触发通知；
//   3) 所有 fd 注册（main.cpp 的 addfd、M_http::modfd）都要带 EPOLLET。
// 把标志集中在这里，两处注册取同一个宏，就不会出现“只改一边”导致的连接卡死。
#define ET_EPOLL

#ifdef ET_EPOLL
#define EPOLL_TRIGGER EPOLLET
#else
#define EPOLL_TRIGGER 0u
#endif

#endif // M_EPOLL_MODE_H
