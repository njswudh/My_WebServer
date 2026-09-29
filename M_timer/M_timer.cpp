#include "M_timer.h"
#include <sys/timerfd.h>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <cstdint>

M_timer::M_timer() : m_timerfd(-1) {
    m_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (m_timerfd < 0) {
        std::cerr << "timerfd_create 失败: " << std::strerror(errno) << std::endl;
        throw std::runtime_error("timerfd_create failed");
    }
}

M_timer::~M_timer() {
    for (auto* p : m_heap) {
        delete p;
    }
    if (m_timerfd >= 0) {
        close(m_timerfd);
    }
}

void M_timer::swap_node(std::size_t a, std::size_t b) {
    std::swap(m_heap[a], m_heap[b]);
    m_heap[a]->slot = a;      // 交换后同步下标，保证 O(log n) 删除/调整
    m_heap[b]->slot = b;
}

void M_timer::sift_up(std::size_t idx) {
    while (idx > 0) {
        std::size_t parent = (idx - 1) / 2;
        if (m_heap[parent]->expire <= m_heap[idx]->expire) {
            break;
        }
        swap_node(parent, idx);
        idx = parent;
    }
}

void M_timer::sift_down(std::size_t idx) {
    std::size_t n = m_heap.size();
    while (true) {
        std::size_t left = idx * 2 + 1;
        std::size_t right = idx * 2 + 2;
        std::size_t smallest = idx;
        if (left < n && m_heap[left]->expire < m_heap[smallest]->expire) {
            smallest = left;
        }
        if (right < n && m_heap[right]->expire < m_heap[smallest]->expire) {
            smallest = right;
        }
        if (smallest == idx) {
            break;
        }
        swap_node(idx, smallest);
        idx = smallest;
    }
}

M_timer_node* M_timer::add(Duration delay, std::function<void()> callback) {
    auto* node = new M_timer_node(std::chrono::steady_clock::now() + delay,
                                  std::move(callback));
    node->slot = m_heap.size();
    m_heap.push_back(node);
    sift_up(node->slot);
    rearm();
    return node;
}

void M_timer::adjust(M_timer_node* node, Duration delay) {
    if (node == nullptr) {
        return;
    }
    node->expire = std::chrono::steady_clock::now() + delay;
    // 刷新只会让到期时间变晚，因此只需下沉
    sift_down(node->slot);
    rearm();
}

void M_timer::del(M_timer_node* node) {
    if (node == nullptr) {
        return;
    }
    std::size_t idx = node->slot;
    swap_node(idx, m_heap.size() - 1);   // 与末尾交换再弹出
    delete node;
    m_heap.pop_back();
    if (idx < m_heap.size()) {           // 被交换上来的元素需要重新定位
        sift_up(idx);
        sift_down(idx);
    }
    rearm();
}

void M_timer::tick() {
    auto now = std::chrono::steady_clock::now();
    while (!m_heap.empty() && m_heap[0]->expire <= now) {
        M_timer_node* top = m_heap[0];
        swap_node(0, m_heap.size() - 1);   // 先移出堆
        m_heap.pop_back();
        if (!m_heap.empty()) {
            sift_down(0);
        }

        // 回调里可能再 add/del/adjust 其他节点；注意不要对 top 自身再操作（它正在被销毁）
        if (top->callback) {
            top->callback();
        }
        delete top;

        now = std::chrono::steady_clock::now();   // 回调耗时，重新取当前时间
    }
    rearm();
}

void M_timer::rearm() {
    struct itimerspec its{};
    if (m_heap.empty()) {
        its.it_value = {0, 0};   // 堆空：停掉 timerfd，避免空转
    } else {
        auto tp = m_heap[0]->expire;   // 绝对到期时间
        auto sec  = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch());
        auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch() - sec);
        its.it_value.tv_sec  = sec.count();
        its.it_value.tv_nsec = nsec.count();
    }
    timerfd_settime(m_timerfd, TFD_TIMER_ABSTIME, &its, nullptr);   // it_interval 保持 0，一次性触发
}

void M_timer::handle_read() {
    std::uint64_t expirations = 0;
    ssize_t n = read(m_timerfd, &expirations, sizeof(expirations));   // 读掉到期计数，否则 fd 会一直可读
    (void)n;
    tick();   // tick 内部已 rearm
}
