#ifndef HEAP_TIMER_H
#define HEAP_TIMER_H

#include <queue>
#include <unordered_map>
#include <time.h>
#include <algorithm>
#include <arpa/inet.h>
#include <functional>
#include <assert.h>
#include <chrono>
#include "../log/log.h"

typedef std::function<void()> TimeoutCallBack;      // 回调函数
typedef std::chrono::high_resolution_clock Clock;   // now()获取当前时间
typedef std::chrono::milliseconds MS;               // 表示毫秒
typedef Clock::time_point TimeStamp;                // 时间点

struct TimerNode {
    int id;             // 标记定时器
    TimeStamp expires;  // 设置过期时间
    TimeoutCallBack cb; // 回调函数
    bool operator<(const TimerNode& t){
        return expires < t.expires;
    }
};

class HeapTimer {
public:
    HeapTimer() { heap_.reserve(64); }
    ~HeapTimer() { clear(); }

    void adjust(int id, int newExpires);
    void add(int id, int timeout, const TimeoutCallBack& cb);
    void doWork(int id);
    void tick();
    void pop();
    void clear();
    int getNextTick();

private:
    void del_(size_t i);                    // 删除定时器
    void siftUp_(size_t i);                 // 向上调整
    bool siftDown_(size_t index, size_t n); // 向下调整
    void SwapNode_(size_t i, size_t j);     // 交换节点

    std::vector<TimerNode> heap_;           // 存储结构
    std::unordered_map<int ,size_t> ref_;   // map<id, vector下标>
};



#endif