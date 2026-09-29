#include"M_sql_pool.h"

M_sql_pool::M_sql_pool()
    : m_max_conn(0),  m_port(0) {
}

M_sql_pool::~M_sql_pool() {
    closePool();
}

//单例模式：C++11 起局部静态变量的初始化是线程安全的
M_sql_pool* M_sql_pool::getInstance() {
    static M_sql_pool instance;
    return &instance;
}

//创建一条数据库连接
MYSQL* M_sql_pool::createoneconn() {
    MYSQL* conn = mysql_init(nullptr);
    if (conn == nullptr) {
        std::cerr << "mysql_init 失败" << std::endl;
        return nullptr;
    }

    
    

    if (mysql_real_connect(conn, m_host.c_str(), m_user.c_str(),
                           m_password.c_str(), m_db_name.c_str(),
                           m_port, nullptr, 0) == nullptr) {
        std::cerr << "mysql_real_connect 失败: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return nullptr;
    }

    //统一使用 utf8mb4，避免中文乱码
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

//初始化连接池：保存配置，并一次性预创建 max_conn 条连接放入队列
bool M_sql_pool::init(std::string host, std::string user, std::string password,
                      std::string db_name, int port, int max_conn) {
    //若之前初始化过，先释放旧连接，避免内存泄漏
   closePool();
   m_poolstop = false;
   
                
   m_host = host;
   m_user = user;
   m_password = password;
   m_db_name = db_name;
   m_port = port;
   m_max_conn = max_conn;

   if(m_max_conn <= 0){
    std::cerr<<"最大连接数 m_max_conn应该大于0"<<std::endl;
    return false;
   }
    
   for(int i = 0;i < m_max_conn;i++){
        MYSQL *conn = createoneconn();
        if(conn == nullptr){
            closePool();
            return false;
        }
        m_conn_queue.emplace(conn);
        ++m_idleCount;

   }
   return true;
}

//从连接池取一条连接，队列为空时阻塞等待
MYSQL* M_sql_pool::getConnection() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if(!m_cond.wait_for(lock,std::chrono::seconds(3),[this]{
            return !m_conn_queue.empty()|| m_poolstop;
        })){
            return nullptr;
    }
    if(m_poolstop) return nullptr;
    MYSQL *conn = m_conn_queue.front();
    m_conn_queue.pop();
    m_idleCount--;

    lock.unlock();

    if(mysql_ping(conn) != 0){
        mysql_close(conn);
        return createoneconn();
    }

    return conn;
}

//把连接归还连接池，并唤醒一个等待连接的线程
void M_sql_pool::releaseConnection(MYSQL* conn) {
    if(conn == nullptr) return ;

   if(mysql_reset_connection(conn) != 0){
        mysql_close(conn);
        conn = createoneconn();
   }

   std::unique_lock<std::mutex> lock(m_mutex);

   if (m_poolstop) {                    // 池已关：销毁，不放回
    if (conn != nullptr)
        mysql_close(conn);
    }   
    else if (conn != nullptr) {
        m_conn_queue.emplace(conn);
        m_idleCount++;
    }
    m_cond.notify_one();
   
    
}

int M_sql_pool::getFreeConn() {
    return m_idleCount;
}

//关闭连接池：唤醒所有等待线程，并关闭队列中的全部连接
void M_sql_pool::closePool() {
    m_poolstop = true;
    m_cond.notify_all();

    std::lock_guard<std::mutex> lock(m_mutex);
    while(!m_conn_queue.empty()){
        auto it = m_conn_queue.front();
        mysql_close(it);
        m_conn_queue.pop();
        m_idleCount--;
    }
}



M_sql_poolRAII::M_sql_poolRAII(MYSQL** M, M_sql_pool* p)
    : Mysql(nullptr), pool(p) {
    if(p == nullptr){
        *M = nullptr;
        return ;
    }
    Mysql = p->getConnection();
    *M = Mysql;
}

M_sql_poolRAII::~M_sql_poolRAII() {
    if(pool != nullptr && Mysql != nullptr)
        pool->releaseConnection(Mysql);
}
