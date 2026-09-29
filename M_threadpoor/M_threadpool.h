#pragma once
#include<string>
#include<thread>
#include<condition_variable>
#include<functional>
#include<queue>
#include<mutex>
#include<future>
#include<vector>
#include<memory>
#include<stdexcept>
#include<utility>
#include<type_traits>


class M_threadpool {
    private:
        std::vector<std::thread> m_threads;
        std::queue<std::function<void()>> m_tasks;
        std::mutex m_mutex;
        std::condition_variable m_cond;
        bool m_stop;
        
    public:
        M_threadpool(size_t thread_count);
        ~M_threadpool();
        void stop();

        


        template<class F, class... Args>
        auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
            using return_type = std::invoke_result_t<F, Args...>;
            auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            std::future<return_type> res = task->get_future();
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_stop)
                    throw std::runtime_error("从线程池中添加任务失败，线程池已停");
                m_tasks.emplace([task]() { (*task)(); });
                m_cond.notify_one();
            }
            return res;
        }

};


M_threadpool::M_threadpool(size_t thread_count) : m_stop(false) {
    for (size_t i = 0; i < thread_count; ++i) {
        m_threads.emplace_back([this]() {
            while (true) {
                std::function<void()> task; 
                {
                    std::unique_lock<std::mutex> lock(this->m_mutex);
                    this->m_cond.wait(lock, [this]() { return this->m_stop || !this->m_tasks.empty(); });
                    if (this->m_stop && this->m_tasks.empty())
                        return;
                    task = std::move(this->m_tasks.front());
                    this->m_tasks.pop();
                }
                task();
            }
        });
    }
}


M_threadpool::~M_threadpool() {
    stop();
    for (std::thread &worker : m_threads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void M_threadpool::stop() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cond.notify_all();
}

