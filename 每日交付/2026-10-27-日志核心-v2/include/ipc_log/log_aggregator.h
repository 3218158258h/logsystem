#ifndef IPC_LOG_LOG_AGGREGATOR_H
#define IPC_LOG_LOG_AGGREGATOR_H

#include "ipc_log/async_queue.h"
#include "ipc_log/log_target.h"
#include "ipc_log/reorder_buffer.h"
#include "ipc_log/sink.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace ipc_log {

struct AggregatorConfig
{
    std::size_t queue_capacity;
    std::size_t control_reserve;
    ReorderConfig reorder;
    //control_reserve 独立于数据容量，避免满载记录挤掉 flush 屏障。
    AggregatorConfig() : queue_capacity(8192), control_reserve(128) {}
};

//主进程本地和远端记录进入同一队列；唯一消费者负责重排、格式化前处理和双 Sink 输出。
class LogAggregator : public ILogTarget
{
public:
    typedef std::function<void(const FlushResult &)> FlushCallback;

    LogAggregator(const std::shared_ptr<ILogSink> &console_sink,
                  const std::shared_ptr<ILogSink> &file_sink,
                  const AggregatorConfig &config = AggregatorConfig());
    ~LogAggregator();

    //尝试入有界记录队列；false 表示关闭/校验失败/队列满，不等待 Sink I/O。
    bool submit(LogRecord record);
    //屏障与记录共用 FIFO；回调在 Sink 刷新结束后运行，不代表 fsync。
    bool flushAsync(FlushCallback callback);
    FlushResult flush(std::chrono::milliseconds timeout);
    //停止接收后排入最后屏障，关闭队列并等待消费者退出。
    void shutdown(std::chrono::milliseconds timeout);
    TargetStats stats() const;

private:
    struct Item
    {
        enum Kind { Record, Barrier } kind;
        LogRecord record;
        FlushCallback callback;
        Item() : kind(Record) {}
    };

    void run();
    FlushResult flushSinks();
    void outputReady(bool force);

    BoundedQueue<Item> queue_;
    std::shared_ptr<ILogSink> console_sink_;
    std::shared_ptr<ILogSink> file_sink_;
    ReorderBuffer reorder_;
    const std::chrono::milliseconds idle_release_;
    std::thread thread_;
    std::atomic<bool> accepting_;
    std::atomic<bool> stopping_;
    uint64_t next_sequence_;
    std::atomic<uint64_t> accepted_;
    std::atomic<uint64_t> dropped_;
    std::atomic<uint64_t> rejected_;
    std::atomic<uint64_t> write_errors_;
    //同步并发生产者的序号分配与入队，并和关闭时停止接收互斥。
    mutable std::mutex submit_mutex_;
    std::timed_mutex flush_mutex_;
    std::mutex shutdown_mutex_;
};

} //namespace ipc_log

#endif
