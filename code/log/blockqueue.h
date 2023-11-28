/*
    阻塞队列
*/

#ifndef BLOCKQUEUE_H
#define BLOCKQUEUE_H

#include <mutex>
#include <deque>
#include <condition_variable>
#include <sys/time.h>
#include <assert.h>

template<typename T>
class BlockDeque{
public:
    // explicit 不让cpp隐式转换
    explicit BlockDeque(size_t MaxCapacity = 1000);
    ~BlockDeque();

    void clear();

    size_t size();
    size_t capacity();

    bool empty();
    bool full();

    void Close();

    T front();
    T back();

    void push_back(const T &item);
    void push_front(const T &item);

    bool pop(T &item);
    bool pop(T &item, int timeout);

    void flush();
private:
    std::deque<T> deq_;                     // 队列
    size_t capacity_;                       // 容量
    std::mutex mtx_;                        // 锁
    bool isClose_;                          // 是否关闭
    std::condition_variable condConsumer_;  // 消费者条件变量
    std::condition_variable condProducer_;  // 生产者条件变量
};

// 构造函数
template<typename T>
BlockDeque<T>::BlockDeque(size_t MaxCapacity) :capacity_(MaxCapacity){
    assert(MaxCapacity > 0);
    isClose_ = false;
}
// 析构函数
template<typename T>
BlockDeque<T>::~BlockDeque() {
    Close();
}
// 析构函数
template<typename T>
void BlockDeque<T>::Close(){
    // 操作前上锁 清理队列中所有成员 唤醒所有阻塞中的生产者消费者线程
    {
        std::lock_guard<std::mutex> locker(mtx_);
        deq_.clear();
        isClose_ = true;
    }
    condProducer_.notify_all();
    condConsumer_.notify_all();
}
// 
template<typename T>
void BlockDeque<T>::clear(){
    std::lock_guard<std::mutex> locker(mtx_);
    deq_.clear();
}
// 唤醒消费者
template<typename T>
void BlockDeque<T>::flush(){
    condConsumer_.notify_one();
}
// 队列大小
template<typename T>
size_t BlockDeque<T>::size(){
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size();
}
// 队列容量
template<typename T>
size_t BlockDeque<T>::capacity(){
    std::lock_guard<std::mutex> locker(mtx_);
    return capacity_;
}
// 是否为空
template<typename T>
bool BlockDeque<T>::empty(){
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.empty();
}
// 是否为满
template<typename T>
bool BlockDeque<T>::full(){
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size() >= capacity_;
}
// 队头
template<typename T>
T BlockDeque<T>::front(){
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.front();
}
// 队尾
template<typename T>
T BlockDeque<T>::back(){
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.back();
}
// 从头插入队列
template<typename T>
void BlockDeque<T>::push_front(const T &item){
    // 使用unique_lock可以临时解锁再加锁，lock_guard上锁后只能在离开作用域解锁
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.size() >= capacity_){
        // 队列已满则阻塞生产者 自动对互斥量解锁，唤醒后自动加锁
        condProducer_.wait(locker);
    }
    deq_.push_front(item);
    condConsumer_.notify_one(); // 唤醒一个阻塞的消费者线程
}
// 从尾插入队列
template<typename T>
void BlockDeque<T>::push_back(const T &item){
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.size() >= capacity_){
        condProducer_.wait(locker);
    }
    deq_.push_back(item);
    condConsumer_.notify_one(); 
}
// pop保证线程安全 使用T&作为删除数据的拷贝
template<typename T>
bool BlockDeque<T>::pop(T &item){
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.empty()){
        // 队列为空 消费者阻塞
        if(isClose_){
            return false;
        }
        condConsumer_.wait(locker);
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

template<typename T>
bool BlockDeque<T>::pop(T &item, int timeout){
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.empty()){
        // 阻塞时间超过timeout则直接返回false
        if(condConsumer_.wait_for(locker, std::chrono::seconds(timeout))
                == std::cv_status::timeout){
            return false;
        }

        if(isClose_){
            return false;
        }
    }

    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

#endif