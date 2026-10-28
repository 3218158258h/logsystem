#include "ipc_log/log_aggregator.h"
#include "ipc_log/log_formatter.h"

#include <condition_variable>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ipc_log {

//本地记录与 FDBus 收到的记录共用此有界队列；唯一后台线程排序、格式化并写两个 Sink。
LogAggregator::LogAggregator(const std::shared_ptr<ILogSink> &console_sink,
                             const std::shared_ptr<ILogSink> &file_sink,
                             const AggregatorConfig &config)
    : queue_(config.queue_capacity, config.control_reserve)
    , console_sink_(console_sink)
    , file_sink_(file_sink)
    , reorder_(config.reorder)
    , idle_release_(config.reorder.idle_release)
    , accepting_(true), stopping_(false), next_sequence_(1)
    , accepted_(0), dropped_(0), rejected_(0), write_errors_(0)
{
    if (config.queue_capacity == 0 || config.queue_capacity > 65536 ||
        config.control_reserve == 0 || config.control_reserve > 4096 ||
        !console_sink_ || !file_sink_)
        throw std::invalid_argument(
            "aggregator requires bounded nonzero queue/control capacities and both sinks");
    thread_ = std::thread(&LogAggregator::run, this);
}

LogAggregator::~LogAggregator()
{
    shutdown(std::chrono::seconds(5));
}

bool LogAggregator::submit(LogRecord record)
{
    //无锁快速拒绝；获取互斥锁后再检查一次，和 shutdown 的停止接收操作同步。
    if (!accepting_.load(std::memory_order_acquire))
    {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::lock_guard<std::mutex> lock(submit_mutex_);
    if (!accepting_.load(std::memory_order_relaxed))
    {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (record.timestamp_ms == 0 || record.pid == 0 || record.process_instance == 0 ||
        !isValidLogLevel(static_cast<uint32_t>(record.level)) || record.module.empty() ||
        record.module.size() > 128 || record.message.size() > 4096)
    {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    //来自 worker 的序号已由发送端分配；本地记录由此队列在入队成功时分配。
    Item item;
    item.kind = Item::Record;
    const bool assign_sequence = record.sequence == 0;
    if (assign_sequence)
        record.sequence = next_sequence_;
    item.record = std::move(record);
    if (!queue_.tryPush(std::move(item)))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (assign_sequence) ++next_sequence_;
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool LogAggregator::flushAsync(FlushCallback callback)
{
    //异步屏障占独立预留容量；队列关闭后拒绝新屏障，但不等待消费线程。
    if (stopping_.load(std::memory_order_acquire)) return false;
    Item item;
    item.kind = Item::Barrier;
    item.callback = std::move(callback);
    return queue_.tryPushControl(std::move(item));
}

FlushResult LogAggregator::flush(std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0)
    {
        FlushResult result;
        result.status = FlushStatus::Timeout;
        result.message = "flush timeout must be positive";
        return result;
    }
    const auto failedFlush = [this](FlushStatus status, const std::string &message) -> FlushResult
    {
        FlushResult result;
        result.status = status;
        result.message = message;
        result.dropped = dropped_.load(std::memory_order_relaxed);
        result.rejected = rejected_.load(std::memory_order_relaxed);
        result.write_errors = write_errors_.load(std::memory_order_relaxed);
        return result;
    };
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::timed_mutex> serial(flush_mutex_, std::defer_lock);
    if (!serial.try_lock_until(deadline))
        return failedFlush(FlushStatus::Timeout, "timed out waiting for another flush operation");
    //同步接口只等待后台线程执行同一条异步屏障，并与其他同步 flush 串行化。
    struct WaitState
    {
        std::mutex mutex;
        std::condition_variable ready;
        bool done;
        FlushResult result;
        WaitState() : done(false) {}
    };
    const std::shared_ptr<WaitState> state(new WaitState());
    if (!flushAsync([state](const FlushResult &result)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result = result;
        state->done = true;
        state->ready.notify_all();
    }))
        return failedFlush(FlushStatus::QueueFull, "flush control reserve is full or target is stopping");

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->ready.wait_until(lock, deadline, [state]() { return state->done; }))
    {
        return failedFlush(FlushStatus::Timeout,
                           "flush timed out; the queued barrier may still complete");
    }
    return state->result;
}

void LogAggregator::shutdown(std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
    if (stopping_.load(std::memory_order_acquire)) return;
    //先停止新增记录，再排入屏障；即使屏障超时也关闭队列并等待消费者完成退出路径。
    {
        std::lock_guard<std::mutex> submit_lock(submit_mutex_);
        accepting_.store(false, std::memory_order_release);
    }
    (void)flush(timeout);
    stopping_.store(true, std::memory_order_release);
    queue_.close();
    if (thread_.joinable()) thread_.join();
}

TargetStats LogAggregator::stats() const
{
    TargetStats result;
    result.accepted = accepted_.load(std::memory_order_relaxed);
    result.dropped = dropped_.load(std::memory_order_relaxed);
    result.rejected = rejected_.load(std::memory_order_relaxed);
    result.write_errors = write_errors_.load(std::memory_order_relaxed);
    result.late_records = reorder_.lateRecords();
    result.forced_releases = reorder_.forcedReleases();
    return result;
}

void LogAggregator::run()
{
    Item item;
    while (true)
    {
        //有限等待让低流量日志也能在空闲释放期限后输出，而不要求新记录到达。
        if (!queue_.waitPopFor(item, idle_release_))
        {
            outputReady(false);
            if (queue_.closed()) break;
            continue;
        }
        if (item.kind == Item::Record)
        {
            reorder_.add(item.record);
            outputReady(false);
        }
        else
        {
            //屏障到达时先强制排空重排窗口，再刷新两个 Sink，最后才应答调用方。
            outputReady(true);
            FlushResult result = flushSinks();
            if (result.status == FlushStatus::Success &&
                write_errors_.load(std::memory_order_relaxed) != 0)
            {
                result.status = FlushStatus::WriteError;
                result.message = "one or more sink writes failed before this flush barrier";
            }
            result.dropped = dropped_.load(std::memory_order_relaxed);
            result.rejected = rejected_.load(std::memory_order_relaxed);
            result.write_errors = write_errors_.load(std::memory_order_relaxed);
            if (result.status == FlushStatus::Success &&
                (result.dropped != 0 || result.rejected != 0))
            {
                result.status = FlushStatus::Partial;
                result.message = "flush completed with dropped or rejected records";
            }
            if (item.callback)
            {
                try { item.callback(result); }
                catch (...) { rejected_.fetch_add(1, std::memory_order_relaxed); }
            }
        }
    }
    outputReady(true);
    (void)flushSinks();
}

void LogAggregator::outputReady(bool force)
{
    //排序释放和 Sink 写入只由消费线程执行。
    std::vector<LogRecord> records;
    reorder_.releaseReady(std::chrono::steady_clock::now(), force, records);
    //先取出当前可释放集合，再逐条格式化；每条同时写 stdout/stderr 规则和轮转文件。
    for (std::vector<LogRecord>::const_iterator it = records.begin(); it != records.end(); ++it)
    {
        const std::string line = LogFormatter::format(*it);
        const SinkResult console_result = console_sink_->write(line);
        const SinkResult file_result = file_sink_->write(line);
        if (!console_result.ok) write_errors_.fetch_add(1, std::memory_order_relaxed);
        if (!file_result.ok) write_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

FlushResult LogAggregator::flushSinks()
{
    //两个 Sink 都尝试 flush；分别累计错误，避免一个 Sink 失败跳过另一个。
    FlushResult result;
    const SinkResult console_result = console_sink_->flush();
    const SinkResult file_result = file_sink_->flush();
    if (!console_result.ok)
    {
        result.status = FlushStatus::WriteError;
        result.message = console_result.message;
        write_errors_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!file_result.ok)
    {
        result.status = FlushStatus::WriteError;
        if (!result.message.empty()) result.message += "; ";
        result.message += file_result.message;
        write_errors_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

} //namespace ipc_log
