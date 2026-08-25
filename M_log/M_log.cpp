#include"M_log.h"
#include<ctime>
#include<filesystem>
#include<string>
#include<cstring>

M_log::M_log():m_is_aync_write(false), m_log_line_count(0), m_log_split_lines(0), m_log_buf(nullptr), m_log_buf_size(0), m_today(0), m_split_index(0){

}


std::string M_log::getNowTime(const char* format) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm{};
    localtime_s(&now_tm, &now_time);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), format, &now_tm);

    return std::string(buffer);
}


bool M_log::init(std::string file_name, int log_buf_size, int split_lines, int max_queue_size){

    m_log_buf_size = log_buf_size;
    m_log_buf = new char[m_log_buf_size];
    std::memset(m_log_buf, '\0', m_log_buf_size);
    m_log_split_lines = split_lines;

    //分离路径与文件名
    namespace fs = std::filesystem;
    fs::path log_path(file_name);
    fs::path dir = log_path.parent_path();
    std::string log_name = log_path.stem().string();
    if (!dir.empty() && !fs::exists(dir)) {
        fs::create_directories(dir);
    }

    m_log_dir = dir.string();
    m_log_base = log_name;
    m_today = std::stoi(getNowTime("%Y%m%d"));
    m_log_name = (dir / (getNowTime("%Y_%m_%d") + "_" + log_name + ".log")).string();

    m_log_file.open(m_log_name, std::ios::app);
    if(!m_log_file.is_open()){
        std::cerr << "文件打开失败: " << m_log_name << std::endl;
        return false;
    }

    if(max_queue_size > 0){
        m_is_aync_write = true;
        m_block_que = std::move(M_block_que<std::string>(max_queue_size));
        m_write_thread = std::thread(&M_log::async_write_log, this);
    }

    return true;
}

void M_log::async_write_log(){
    std::string log_str;
    while(m_block_que.pop(log_str)){
        
        int today = std::stoi(getNowTime("%Y%m%d"));
        if(m_today != today || (m_log_split_lines > 0 && m_log_line_count >= m_log_split_lines)){
            open_new_log_file();
        }

        m_log_file << log_str << '\n';
        m_log_line_count++;
        m_log_file.flush();
    }
}

M_log::~M_log(){
    m_block_que.close();   //  关闭队列，唤醒并停止消费者线程
    if(m_write_thread.joinable()){
        m_write_thread.join();
    }
    if(m_log_file.is_open()){
        m_log_file.close();
    }
    delete[] m_log_buf;
}

void M_log::flush(void){
    if(m_is_aync_write){
        // 异步模式：写线程每行都已 flush，这里无需额外处理
        return;
    }
    else{
        std::lock_guard<std::mutex> lock(m_mutex);
        m_log_file.flush();
    }
}

void M_log::open_new_log_file() {
    int today = std::stoi(getNowTime("%Y%m%d"));
    m_log_file.flush();
    m_log_file.close();

    namespace fs = std::filesystem;
    fs::path dir(m_log_dir);

    if(m_today != today){   // 跨天：新的一天
        m_today = today;
        m_log_line_count = 0;
        m_split_index = 0;
        m_log_name = (dir / (getNowTime("%Y_%m_%d") + "_" + m_log_base + ".log")).string();
    } else {                // 超行数：加序号
        m_split_index++;
        m_log_line_count = 0;
        m_log_name = (dir / (getNowTime("%Y_%m_%d") + "_" + m_log_base + "_" + std::to_string(m_split_index) + ".log")).string();
    }

    m_log_file.open(m_log_name, std::ios::app);
    if(!m_log_file.is_open()){
        std::cerr << "文件打开失败: " << m_log_name << std::endl;
    }
}
