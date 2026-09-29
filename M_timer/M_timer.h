#ifndef M_TIMER_H
#define M_TIMER_H

#include <chrono>
#include <functional>
#include <vector>
#include <cstddef>

// 定时器节点：一个绝对到期时间 + 一个到期回调
struct M_timer_node {
    using TimePoint = std::chrono::steady_clock::time_point;

    TimePoint expire;                    // 绝对到期时间（单调时钟）
    std::function<void()> callback;      // 到期后执行的回调
    std::size_t slot;                    // 在最小堆数组中的下标，用于 O(log n) 调整/删除

    M_timer_node() = default;
    M_timer_node(TimePoint t, std::function<void()> cb)
        : expire(t), callback(std::move(cb)), slot(0) {}
};

// 定时器：最小堆 + timerfd
// 线程模型：由单线程驱动（未来的 epoll 主循环），内部不加锁；
// add/del/adjust/tick 必须在同一个线程里调用。
class M_timer {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration  = std::chrono::milliseconds;

    M_timer();
    ~M_timer();

    M_timer(const M_timer&) = delete;
    M_timer& operator=(const M_timer&) = delete;

    // 添加：delay 毫秒后执行 callback；返回节点指针，供之后 adjust/del 使用
    M_timer_node* add(Duration delay, std::function<void()> callback);

    // 刷新：把节点到期时间重置为“从现在起 delay 毫秒”（连接有活动时调用）
    void adjust(M_timer_node* node, Duration delay);

    // 删除节点（连接正常关闭时调用）
    void del(M_timer_node* node);

    // 到点检查：执行所有已到期定时器的回调（内部会重设 timerfd）
    void tick();

    // 定时器 fd，供外部 epoll 注册（events = EPOLLIN | EPOLLET）
    int fd() const { return m_timerfd; }

    // timerfd 可读时调用：读掉到期计数 -> tick
    void handle_read();

    bool empty() const { return m_heap.empty(); }
    std::size_t size() const { return m_heap.size(); }

private:
    void sift_up(std::size_t idx);
    void sift_down(std::size_t idx);
    void swap_node(std::size_t a, std::size_t b);   // 交换并同步 slot
    void rearm();                                    // 根据堆顶重设 timerfd

private:
    std::vector<M_timer_node*> m_heap;   // 最小堆，按 expire 升序
    int m_timerfd;                       // timerfd 文件描述符
};

#endif // M_TIMER_H
