#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
#include <assert.h>

class ThreadPool{
public:
    explicit ThreadPool(size_t threadCount = 8): pool_(std::make_shared<Pool>()){
        assert(threadCount > 0);
        for(size_t i = 0; i < threadCount; i++){
            std::thread([pool = pool_] {
                std::unique_lock<std::mutex> locker(pool->mtx);
                while(true){
                    if(!pool->tasks.empty()){
                        auto task = std::move(pool->tasks.front());
                        pool->tasks.pop();
                        locker.unlock();    // 任务已取出 可提前解锁
                        task();             // 回调函数
                        locker.lock();      // 马上又要取任务 上锁
                    }
                    else if(pool->isClosed) break;
                    else pool->cond.wait(locker);   // 等待 任务来了就notify
                }
            }).detach();
        }
    }

    ThreadPool() = default;
    ThreadPool(ThreadPool&&) = default;

    ~ThreadPool(){
        if(static_cast<bool>(pool_)){
            {
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            pool_->cond.notify_all();       // 唤醒所有进程 处理剩下任务
        }
    }

    template<typename T>
    void AddTask(T&& task){
        {
            std::unique_lock<std::mutex> locker(pool_->mtx);
            pool_->tasks.emplace(std::forward<T>(task));
        }
        pool_->cond.notify_all();
    }
    

private:
    struct Pool{
        std::mutex mtx;
        std::condition_variable cond;
        bool isClosed;
        std::queue<std::function<void()>> tasks;    // 任务队列
    };
    std::shared_ptr<Pool> pool_;        
};

#endif