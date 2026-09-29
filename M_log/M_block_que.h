//用于异步日志的阻塞队列


#include<deque>
#include<mutex>
#include<condition_variable>
template<typename T>
class M_block_que{
    private:
        std::mutex m_mutex;
        std::condition_variable m_cond_not_full;//生产者信号量
        std::condition_variable m_cond_not_empty;//消费者信号量
        std::deque<T> m_deque;
        int m_que_size;
        bool m_closed = false;
    public:
        M_block_que(int capacity = 0): m_que_size(capacity) {}
        M_block_que(const M_block_que&) = delete;
        M_block_que& operator=(const M_block_que&) = delete;

        M_block_que(M_block_que&& other) noexcept
            : m_mutex(),
              m_cond_not_full(),
              m_cond_not_empty(),
              m_deque(std::move(other.m_deque)),
              m_que_size(other.m_que_size) {}

        M_block_que& operator=(M_block_que&& other) noexcept {
            if (this != &other) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_deque = std::move(other.m_deque);
                m_que_size = other.m_que_size;
                m_closed = other.m_closed;
            }
            return *this;
        }

        // 关闭队列，唤醒所有等待的生产者/消费者
        void close(){
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cond_not_empty.notify_all();
            m_cond_not_full.notify_all();
        }
        bool push(const T& item){
            std::unique_lock<std::mutex> lock(m_mutex);
            while(m_deque.size() >= m_que_size && !m_closed){
                m_cond_not_full.wait(lock, [this]{return m_deque.size() < m_que_size || m_closed;});
            }
            if(m_closed){
                return false;
            }
            m_deque.push_back(item);
            lock.unlock();
            m_cond_not_empty.notify_one();
            return true;
        }
        bool pop(T& item){
            std::unique_lock<std::mutex> lock(m_mutex);
            while(m_deque.empty() && !m_closed){
                m_cond_not_empty.wait(lock, [this]{return !m_deque.empty() || m_closed;});
            }
            if(m_closed && m_deque.empty()){
                return false;   // 已关闭且无数据 → 通知调用方退出
            }
            item = m_deque.front();
            m_deque.pop_front();
            lock.unlock();
            m_cond_not_full.notify_one();
            return true;
        }

        T front(){
            std::unique_lock<std::mutex> lock(m_mutex);
            while(m_deque.empty()){
                m_cond_not_empty.wait(lock, [this]{return !m_deque.empty();});
            }
            return m_deque.front();
        }
        T back(){
            std::unique_lock<std::mutex> lock(m_mutex);
            while(m_deque.empty()){
                m_cond_not_empty.wait(lock, [this]{return !m_deque.empty();});
            }
            return m_deque.back();
        }
        int size(){
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_deque.size();
        }

        void clear(){
            std::lock_guard<std::mutex> lock(m_mutex);
            m_deque.clear();
        }

        bool empty(){
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_deque.empty();
        }

        bool full(){
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_deque.size() >= m_que_size;
        }
        ~M_block_que(){
            std::lock_guard<std::mutex> lock(m_mutex);
            m_deque.clear();
        }
};

