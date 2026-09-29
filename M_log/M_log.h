#pragma once
#include<iostream>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<chrono>
#include<deque>
#include<string>
#include<cstring>
#include<cstdio>
#include<fstream>
#include"M_block_que.h"

class M_log{


    public:

    M_log(const M_log&) = delete;
    M_log& operator=(const M_log&) = delete;
    //获取日志实例
        static M_log* get_instance(){
            static M_log instance;
            return &instance;
        }

    //初始化日志
        bool init(std::string file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    //写日志
        template<typename ...Args>
        void write_log(int level, const char* format, Args ...args);
    //刷新缓冲区
        void flush(void);

        std::string getNowTime(const char* format = "%Y_%m_%d--%H_%M_%S");


    private:
        M_log();
        ~M_log();
        void async_write_log();

        void open_new_log_file();

    private:
        M_block_que<std::string> m_block_que;
        std::thread m_write_thread;//写日志线程
        bool m_is_aync_write;//是否异步写日志
        std::mutex m_mutex;
        std::condition_variable m_cond;
        int m_today;

        std::ofstream m_log_file;//日志文件
        int m_log_line_count;//日志行数记录
        int m_log_split_lines;//日志最大行数
        char* m_log_buf;//日志缓冲区
        int m_log_buf_size;//日志缓冲区大小
        std::string m_log_name;//日志文件名
        std::string m_log_dir;//日志目录
        std::string m_log_base;//基础文件名(不含扩展名)
        int m_split_index;//按行分割的序号
};



template<typename ...Args>
void M_log::write_log(int level, const char* format, Args ...args){
    char Lev[16];
    switch (level)
    {
    case 0:
        std::strcpy(Lev, "[debug]:");
        break;
    case 1:
        std::strcpy(Lev, "[info]:");
        break;
    case 2:
        std::strcpy(Lev, "[warn]:");
        break;
    case 3:
        std::strcpy(Lev, "[error]:");
        break;

    default:
        std::strcpy(Lev, "[info]:");
        break;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    //写入日志内容
    if(m_is_aync_write){
        // 异步：整行写进 m_log_buf，再入队
        std::snprintf(m_log_buf, m_log_buf_size, "%s %s ", getNowTime("%Y-%m-%d %H:%M:%S").c_str(), Lev);
        int len = std::strlen(m_log_buf);
        std::snprintf(m_log_buf + len, m_log_buf_size - len, format, args...);
        m_block_que.push(std::string(m_log_buf));
    }
    else{
        // 同步：切割判断 -> 写文件 -> 行数自增
        int today = std::stoi(getNowTime("%Y%m%d"));
        if(m_today != today || (m_log_split_lines > 0 && m_log_line_count >= m_log_split_lines)){
            open_new_log_file();
        }
        std::snprintf(m_log_buf, m_log_buf_size, "%s %s ", getNowTime("%Y-%m-%d %H:%M:%S").c_str(), Lev);
        int len = std::strlen(m_log_buf);
        std::snprintf(m_log_buf + len, m_log_buf_size - len, format, args...);
        m_log_file << m_log_buf << std::endl;
        m_log_line_count++;
    }
}


#define LOG_DEBUG(format, ...) M_log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) M_log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) M_log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) M_log::get_instance()->write_log(3, format, ##__VA_ARGS__)