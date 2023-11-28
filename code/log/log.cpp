#include "log.h"

Log::Log(){
    lineCouunt_ = 0;
    isAsync_ = false;
    writeThread_ = nullptr;
    deque_ = nullptr;
    toDay_ = 0;
    fp_ = nullptr;
}

Log::~Log(){
    if(writeThread_ && writeThread_->joinable()){
        while(!deque_->empty()){
            deque_->flush();        // 通知剩下消费者进程处理完任务
        }
        deque_->Close();            // 关闭队列
        writeThread_->join();       // 等待当前线程完成任务
    }
    if(fp_){
        std::lock_guard<std::mutex> locker(mtx_);
        flush();
        fclose(fp_);
    }
}

// 初始化日志实例
void Log::init(int level, const char* path, const char* suffix, int maxQueCapacity){
    isOpen_ = true;
    level_ = level;
    path_ = path;
    suffix_ = suffix;
    if(maxQueCapacity){         // 异步模式
        isAsync_ = true;
        if(!deque_){
            std::unique_ptr<BlockDeque<std::string>> newQue(new BlockDeque<std::string>);
            deque_ = move(newQue);  // unique_ptr不支持普通拷贝或复制操作 使用move

            std::unique_ptr<std::thread> newThread(new std::thread(FlushLogThread));
            writeThread_ = move(newThread);
        }
    }else{
        isAsync_ = false;       // 非异步模式
    }
    
    lineCouunt_ = 0;
    time_t timer = time(nullptr);
    struct tm* systime = localtime(&timer);
    char fileName[LOG_NAME_LEN] = {0};
    snprintf(fileName, LOG_NAME_LEN-1, "%s/%04d_%02d_%02d%s", path_, systime->tm_year + 1900,
            systime->tm_mon + 1, systime->tm_mday, suffix_);
    toDay_ = systime->tm_mday;

    {
        std::lock_guard<std::mutex> locker(mtx_);
        buff_.RetrieveAll();
        if(fp_){        // 重新打开
            flush();
            fclose(fp_);
        }

        fp_ = fopen(fileName, "a");
        if (fp_ == nullptr){
            mkdir(path_, 0777);
            fp_ = fopen(fileName, "a");
        }
        assert(fp_ != nullptr);
    }
}
// 写日志
void Log::write(int level, const char* format, ...){
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr);
    time_t tSec = now.tv_sec;       // 当前UTC时间
    struct tm *sysTime = localtime(&tSec);
    struct tm t = *sysTime;
    va_list vaList;                 // 可变参数

    // 日志日期 日志行数 如果不是今天或者行数超了
    if (toDay_ != t.tm_mday || (lineCouunt_ && (lineCouunt_ % MAX_LINES == 0))){
        std::unique_lock<std::mutex> locker(mtx_);
        locker.unlock();

        char newFile[LOG_NAME_LEN];
        char tail[36] = {0};
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        if(toDay_ != t.tm_mday){    // 时间不同 换成最新的日志文件文件名
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s", path_, tail, suffix_);
            toDay_ = t.tm_mday;
            lineCouunt_ = 0;
        }else{                      // 时间相同，行数超过，创建一个额外的文件存储日志
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s",path_, tail, (lineCouunt_ / MAX_LINES), suffix_);
        }

        locker.lock();
        flush();
        fclose(fp_);
        fp_ = fopen(newFile, "a");
        assert(fp_ != nullptr);
    }
    // 在buffer内生成一条日志消息
    {
        std::unique_lock<std::mutex> locker(mtx_);
        lineCouunt_++;
        int n = snprintf(buff_.BeginWrite(), 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);

        buff_.HasWritten(n);
        AppendLogLevelTitle_(level);

        va_start(vaList, format);   // 初始化变长参数列表
        int m = vsnprintf(buff_.BeginWrite(), buff_.WritableBytes(), format, vaList);
        va_end(vaList);             // 结束变长参数列表的访问

        buff_.HasWritten(m);
        buff_.Append("\n\0", 2);
        // 异步方式 加入阻塞队列等待写线程读取日志信息
        if(isAsync_ && deque_ && !deque_->full()){
            deque_->push_back(buff_.RetrieveAllToStr());
        }else{  // 同步方式 直接向文件中写入日志
            fputs(buff_.Peek(), fp_);
        }
        buff_.RetrieveAll();        // 清空buff
    }
}

// 异步日志的写线程函数
void Log::FlushLogThread() {
    Log::Instance()->AsyncWrite_();
}

// 写线程的执行函数
void Log::AsyncWrite_() {
    std::string str = "";
    while(deque_->pop(str)){
        std::lock_guard<std::mutex> locker(mtx_);
        fputs(str.c_str(), fp_);
    }
}

// 懒汉模式 局部静态变量法实现线程安全  
Log* Log::Instance() {
    static Log log;
    return &log;
}

void Log::flush() {
    if (isAsync_){
        deque_->flush();    // 异步日志才需要deque
    }
    fflush(fp_);            // 刷新输入缓冲区
}
// 添加日志等级
void Log::AppendLogLevelTitle_(int level){
    switch (level)
    {
    case 0:
        buff_.Append("[debug]: ", 9);
        break;
    case 1:
        buff_.Append("[info] : ", 9);
        break;
    case 2:
        buff_.Append("[warn] : ", 9);
        break;
    case 3:
        buff_.Append("[error]: ", 9 );
        break;
    default:
        buff_.Append("[info] : ", 9);
        break;
    }
}

int Log::GetLevel() {
    std::lock_guard<std::mutex> locker(mtx_);
    return level_;
}

void Log::SetLevel(int level) {
    std::lock_guard<std::mutex> locker(mtx_);
    level_ = level;
}