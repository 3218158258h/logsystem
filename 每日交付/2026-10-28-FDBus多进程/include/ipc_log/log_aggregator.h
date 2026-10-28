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
#include <memory>
#include <mutex>
#include <thread>

namespace ipc_log {

struct AggregatorConfig//队列配置
{
    std::size_t queue_capacity;
    ReorderConfig reorder;//重排缓冲区配置
    AggregatorConfig() : queue_capacity(8192) {}
};

//
//主进程本地和远端记录进入同一队列；唯一消费者负责重排、格式化前处理和双 Sink 输出。
class LogAggregator : public ILogTarget
{
public:
    LogAggregator(const std::shared_ptr<ILogSink> &console_sink,
                  const std::shared_ptr<ILogSink> &file_sink,
                  const AggregatorConfig &config = AggregatorConfig());
    ~LogAggregator();

    //尝试入有界记录队列；false 表示关闭/队列满，不等待 Sink I/O。
    bool submit(LogRecord record);
    //停止接收后关闭队列并等待消费者排空；消费者的退出路径会刷新两个 Sink。
    ShutdownResult shutdown();
    TargetStats stats() const;

private:
    void run();
    void flushSinks();
    void outputReady(bool force);

    BoundedQueue<LogRecord> queue_;
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
    std::mutex shutdown_mutex_;
};

} //namespace ipc_log

#endif
