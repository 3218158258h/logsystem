#ifndef IPC_LOG_ASYNC_QUEUE_H
#define IPC_LOG_ASYNC_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace ipc_log {

//简单的有界 FIFO：关闭只阻止新项，消费者仍会取完关闭前已入队的项。
template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity), size_(0), closed_(false)
    {}

    //存入
    bool tryPush(T value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || size_ >= capacity_) return false;
        queue_.push_back(std::move(value));
        ++size_;
        ready_.notify_one();//条件变量通知
        return true;
    }
    //取出（无超时）
    bool waitPop(T &value)
    {
        //仅队列关闭且已排空时返回 false；关闭不会丢弃已经入队的项。
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this]() { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        popFront(value);
        return true;
    }
    //取出（有超时）
    template <typename Rep, typename Period>//时间模板
    bool waitPopFor(T &value, const std::chrono::duration<Rep, Period> &duration)
    {
        //false 代表超时或关闭后队列为空；超时由消费者用于推进重排空闲释放。
        std::unique_lock<std::mutex> lock(mutex_);
        if (!ready_.wait_for(lock, duration,
                             [this]() { return closed_ || !queue_.empty(); }))
            return false;
        if (queue_.empty()) return false;
        popFront(value);
        return true;
    }

    void close()
    {
        //阻止新项并唤醒所有等待者；消费者仍可取完关闭前积压的队列项。
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    void popFront(T &value)
    {
        value = std::move(queue_.front());
        queue_.pop_front();
        --size_;
    }

    const std::size_t capacity_;
    //等待消费者时会释放此锁，生产者只在队列操作期间持锁。
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> queue_;
    std::size_t size_;
    bool closed_;
};

} //namespace ipc_log

#endif
