// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef UXR_AGENT_SCHEDULER_FCFS_SCHEDULER_HPP_
#define UXR_AGENT_SCHEDULER_FCFS_SCHEDULER_HPP_

#include <uxr/agent/scheduler/Scheduler.hpp>

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace eprosima {
namespace uxr {
// 先来先服务调度器
template<class T>
class FCFSScheduler : public Scheduler<T>
{
public:
    FCFSScheduler(
            size_t max_size)    // 调度器可调度最大任务数
        : deque_()          // 双端队列
        , mtx_()            // 互斥锁
        , cond_var_()       // 条件变量
        , running_cond_(false)  // 运行条件
        , max_size_{max_size}   //最大任务
    {}

    void init() final;  

    void deinit() final;

    void push(
            T&& element,
            uint8_t priority) final;

    void push_front(
            T&& element);

    bool pop(
            T& element) final;

private:
    std::deque<T> deque_;   // 双端对列
    std::mutex mtx_;        // 互斥锁
    std::condition_variable cond_var_;  // 条件变量
    bool running_cond_;     // 运行条件
    const size_t max_size_; // 常数：最大量
};

template<class T>
inline void FCFSScheduler<T>::init()
{
    std::lock_guard<std::mutex> lock(mtx_); // 加锁
    running_cond_ = true;                   // 更改运行条件为真
}

template<class T>
inline void FCFSScheduler<T>::deinit()
{
    std::lock_guard<std::mutex> lock(mtx_); // 加锁
    running_cond_ = false;                  // 更改运行条件为假
    cond_var_.notify_one();                 // 随机唤醒一个线程 其获得锁
}

template<class T>
inline void FCFSScheduler<T>::push(
        T&& element,
        uint8_t priority)
{
    (void) priority;
    std::lock_guard<std::mutex> lock(mtx_);     // 加锁
    if (max_size_ <= deque_.size())             // 如果队列中的人物大于等于最大容量
    {
        deque_.pop_front();                     // pop出队首元素
    }
    deque_.push_back(std::move(element));       // 任务入队
    cond_var_.notify_one();                     // 随机唤醒一个线程 其获得锁
}

template<class T>
inline void FCFSScheduler<T>::push_front(
        T&& element)
{
    std::lock_guard<std::mutex> lock(mtx_);     // 加锁
    deque_.push_front(std::forward<T>(element));    // 放入队列
}
/**
 * element会获取双端队列队首任务
 **/
template<class T>
inline bool FCFSScheduler<T>::pop(
        T& element)
{
    bool rv = false;
    std::unique_lock<std::mutex> lock(mtx_);    // 上锁
    // 当后面的lambda函数为假时陷入阻塞，直到它为真解除阻塞
    cond_var_.wait(lock, [this] { return !(deque_.empty() && running_cond_); });    //  
    if (running_cond_)  //为真 初始化过了
    {
        element = std::move(deque_.front()); //将双端队列队首元素移动给element
        deque_.pop_front();                 // 出队
        rv = true;                          // 更改返回结果
        cond_var_.notify_one();             // 唤醒一个线程
    }
    return rv;
}

} // namespace uxr
} // namespace eprosima

#endif // UXR_AGENT_SCHEDULER_FCFS_SCHEDULER_HPP_
