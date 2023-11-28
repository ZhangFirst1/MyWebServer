/*
    日志系统
*/

#ifndef LOG_H
#define LOG_H

#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>           // vastart va_end
#include <assert.h>
#include <sys/stat.h>         // mkdir
#include "blockqueue.h"
#include "../buffer/buffer.h"

class Log{
public:
    // 日志保存路径 日志后缀 阻塞队列最大容量
    void init(int level, const char* path = "./log", const char* suffix = ".log",
        int maxQueueCapacity = 1024);
    
    static Log* Instance();         // 获取实例
    static void FlushLogThread();   // 异步写日志，调用私有方法asyncWrite

    int GetLevel();
    void SetLevel(int level);
    bool IsOpen() {return isOpen_;}

    void write(int level, const char* format, ...); // 写入日志
    void flush();                   // 唤醒阻塞队列消费者，开始写日志

private:
    // 使用单例模式 构造和析构放入private
    Log();
    virtual ~Log();
    void AsyncWrite_();             // 异步写日志
    void AppendLogLevelTitle_(int level);   // 增加日志等级

private:
    static const int LOG_PATH_LEN = 256;    // 日志文件最长文件名
    static const int LOG_NAME_LEN = 256;    // 日志最长名
    static const int MAX_LINES = 50000;     // 日志内最长日志条数

    const char* path_;                      // 路径  
    const char* suffix_;                    // 后缀

    int MAX_LINES_;                         // 最大行
    int toDay_;                             // 按当天日期区分文件
    int lineCouunt_;                        // 日志行数记录

    bool isOpen_;
    
    Buffer buff_;                           // 输出内容的缓冲区
    int level_;                             // 日志等级
    bool isAsync_;                          // 是否为异步日志

    FILE* fp_;                                        // 文件指针
    std::unique_ptr<BlockDeque<std::string>> deque_;  // 阻塞队列
    std::unique_ptr<std::thread> writeThread_;        // 写线程的指针
    std::mutex mtx_;                                  // 同步日志互斥量

};

#define LOG_BASE(level, format, ...) \
    do {\
        Log* log = Log::Instance();\
        if (log->IsOpen() && log->GetLevel() <= level) {\
            log->write(level, format, ##__VA_ARGS__); \
            log->flush();\
        }\
    } while(0);

#define LOG_DEBUG(format, ...) do {LOG_BASE(0, format, ##__VA_ARGS__)} while(0);
#define LOG_INFO(format, ...) do {LOG_BASE(1, format, ##__VA_ARGS__)} while(0);
#define LOG_WARN(format, ...) do {LOG_BASE(2, format, ##__VA_ARGS__)} while(0);
#define LOG_ERROR(format, ...) do {LOG_BASE(3, format, ##__VA_ARGS__)} while(0);


#endif