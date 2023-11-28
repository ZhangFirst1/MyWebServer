#include "sqlconnpool.h"
using namespace std;

SqlConnPool::SqlConnPool(){
    useCount_ = 0;
    freeCount_ = 0;
}

SqlConnPool* SqlConnPool::Instance(){
    static SqlConnPool connPool;
    return &connPool;
}

void SqlConnPool::Init(const char* host, int port, const char* user,
            const char* pwd, const char* dbName, int connSize = 10){
    assert(connSize > 0);
    for (int i=0; i < connSize; i++){
        MYSQL* sql = nullptr;
        sql = mysql_init(sql);
        if (!sql){
            LOG_ERROR("MySql init error!");
            assert(sql);
        }
        sql = mysql_real_connect(sql, host, user, pwd, dbName, port, nullptr, 0);
        if (!sql){
            LOG_ERROR("Mysql Connect error!");
        }
        connQue_.push(sql);
    }
    MAX_CONN_ = connSize;
    sem_init(&semId_, 0, MAX_CONN_);        // 创建信号量 用于多线程间同步
}

MYSQL* SqlConnPool::GetConn() {
    MYSQL* sql = nullptr;
    if(connQue_.empty()){
        LOG_WARN("SqlConnPool busy!");
        return nullptr;
    }
    sem_wait(&semId_);                      // -1
    {
        lock_guard<mutex> locker(mtx_);
        sql = connQue_.front();
        connQue_.pop();
    }
    return sql;
}
// 存入线程池 但并没有关闭
void SqlConnPool::FreeConn(MYSQL* sql){
    assert(sql);
    lock_guard<mutex> locker(mtx_);
    connQue_.push(sql);
    sem_post(&semId_);                      // sem + 1唤醒等待该信号量的任意线程
}

void SqlConnPool::ClosePool(){
    lock_guard<mutex> locker(mtx_);
    while(!connQue_.empty()){
        auto item = connQue_.front();
        connQue_.pop();
        mysql_close(item);
    }
    mysql_library_end();
}

int SqlConnPool::GetFreeConnCount(){
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}

SqlConnPool::~SqlConnPool(){
    ClosePool();
}