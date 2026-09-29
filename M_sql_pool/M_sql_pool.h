#ifndef M_SQL_POOL_H
#define M_SQL_POOL_H

#include<mariadb/mysql.h>
#include<thread>
#include<mutex>
#include<queue>
#include<functional>
#include<condition_variable>
#include<iostream>
#include<string>
#include<error.h>
#include<atomic>
#include<chrono>
//数据库连接池类
class M_sql_pool{

private:
    M_sql_pool();
    ~M_sql_pool();

    MYSQL* createoneconn();//创建一条连接
    
public:
    M_sql_pool(const M_sql_pool&) = delete;
    M_sql_pool& operator=(const M_sql_pool&) = delete;

    //单例模式 获取实例
    static M_sql_pool* getInstance();
    //获取单个数据库连接
    MYSQL* getConnection();
    //释放连接回连接池
    void releaseConnection(MYSQL* conn);
    //获取当前空闲的连接数
    int getFreeConn();
    
    //关闭连接池
    void closePool();
    //初始化数据库连接池
    bool init(std::string host, std::string user, std::string password, std::string db_name, int port, int max_conn);

private:
    int m_max_conn; //最大连接数
    
    std::atomic<int> m_idleCount{0};  //当前空闲的连接数
    std::atomic<bool> m_poolstop{false};  //当前池子是否关闭
    std::queue<MYSQL*> m_conn_queue; //连接池队列
    std::mutex m_mutex; //互斥锁
    std::condition_variable m_cond; //条件变量
    
private:
    std::string m_host; //数据库地址
    std::string m_user; //数据库用户名
    std::string m_password; //数据库密码
    std::string m_db_name; //数据库名
    int m_port; //数据库端口号
    







};




//数据库RAII类
class M_sql_poolRAII{
public:
    M_sql_poolRAII(MYSQL** M, M_sql_pool* p);
    ~M_sql_poolRAII();

private:
    MYSQL *Mysql;
    M_sql_pool *pool;

};








#endif
