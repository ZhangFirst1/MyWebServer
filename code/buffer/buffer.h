#ifndef BUFFER_H
#define BUFFER_H

#include <cstring>
#include <iostream>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/uio.h>
#include <assert.h>

class Buffer{
public:
    // 构造和析构函数
    Buffer(int initBuffSize = 1024);
    ~Buffer() = default;
    
    /* buffer分成三个区 分别是prependable、readable、writable 
        prependable可以用很小的代价在数据前添加几个新字节
        readable 读入缓冲区
        writable 写出缓冲区
        当外部写入fd时 将readable写入fd
             读取fd时 读writable
    */
    // 计算三个区域的大小
    size_t WritableBytes()      const;
    size_t ReadableBytes()      const;
    size_t PrependableByters()  const;
    
    const char* Peek() const;               // 返回readPos的地址
    void EnsureWriteable(size_t len);       // 确定写出缓冲区空间是否足够 不足则扩容
    void HasWritten(size_t len);            // 改变writePos_的位置

    void Retrieve(size_t len);              // 改变readPos位置 根据len
    void RetrieveUntil(const char* end);    // 读入缓冲区 直到end

    void RetrieveAll();                     // 读入缓冲区清零
    std::string RetrieveAllToStr();         // 读入缓冲区清零并返回内容

    char* BeginWrite();                     // 写出缓冲区开始地址
    const char* BeginWriteConst() const;    

    void Append(const std::string& str);    // 由ReadFd调用 用于将在栈上的数据写入vector 会调用MakeSpace扩容 
    void Append(const char* str, size_t len);
    void Append(const void* data, size_t len);
    void Append(const Buffer& buff);

    // 主要函数 上面函数多被这两个调用
    ssize_t ReadFd(int fd, int* Errno);     // 从外部向缓冲区内部读入
    ssize_t WriteFd(int fd, int* Errno);    // 从外部写入缓冲区

public:
    char* BeginPtr_();                      // 开始位置
    const char* BeginPtr_() const; 
    void MakeSpace_(size_t len);            // 扩容vector

    std::vector<char> buffer_;              // vector存储实体
    std::atomic<std::size_t> readPos_;      // 当前读的位置 atomic是原子类型 保证多线程情况下安全
    std::atomic<std::size_t> writePos_;     // 当前写的位置

};


#endif 