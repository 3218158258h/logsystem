#include "ipc_log/log_aggregator.h"
#include "ipc_log/log_formatter.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ipc_log {

//本地记录与 FDBus 收到的记录共用此有界队列；唯一后台线程排序、格式化并写两个 Sink。
LogAggregator::LogAggregator(const std::shared_ptr<ILogSink> &console_sink,
                             const std::shared_ptr<ILogSink> &file_sink,
                             const AggregatorConfig &config)
    : queue_(config.queue_capacity)
    , console_sink_(console_sink)
    , file_sink_(file_sink)
    , reorder_(config.reorder)
    , idle_release_(config.reorder.idle_release)
    , accepting_(true), stopping_(false), next_sequence_(1)
    , accepted_(0), dropped_(0), rejected_(0), write_errors_(0)
{
    if (config.queue_capacity == 0 || config.queue_capacity > 65536 ||
        !console_sink_ || !file_sink_)
        throw std::invalid_argument(
            "aggregator requires a bounded nonzero queue and both sinks");
    thread_ = std::thread(&LogAggregator::run, this);
}

LogAggregator::~LogAggregator()
{
    shutdown();
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
    //来自 worker 的序号已由发送端分配；本地记录由汇聚队列在入队成功时分配。
    const bool assign_sequence = record.sequence == 0;
    if (assign_sequence)
        record.sequence = next_sequence_;
    if (!queue_.tryPush(std::move(record)))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (assign_sequence) ++next_sequence_;
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ShutdownResult LogAggregator::shutdown()
{
    //消费线程不能 join 自己；这种调用直接报告未完成，由外部线程再次收尾。
    if (std::this_thread::get_id() == thread_.get_id())
    {
        ShutdownResult result;
        result.drained = false;
        result.message = "shutdown cannot run on the consumer thread";
        return result;
    }
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
    if (stopping_.load(std::memory_order_acquire))
    {
        ShutdownResult result;
        result.already_stopped = true;
        result.message = "aggregator is already stopped";
        result.stats = stats();
        return result;
    }
    //先停止新增记录，再关闭队列；消费者会把关闭前已入队的记录全部处理完。
    {
        std::lock_guard<std::mutex> submit_lock(submit_mutex_);
        accepting_.store(false, std::memory_order_release);
    }
    stopping_.store(true, std::memory_order_release);
    queue_.close();
    if (thread_.joinable()) thread_.join();

    //消费者退出前已强制释放重排窗口并刷新两个 Sink，这里只汇总结果。
    ShutdownResult result;
    result.stats = stats();
    if (result.stats.write_errors != 0)
        result.message = "one or more sink writes failed during final drain";
    return result;
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
    LogRecord record;
    while (true)
    {
        //有限等待让低流量日志也能在空闲释放期限后输出，而不要求新记录到达。
        if (!queue_.waitPopFor(record, idle_release_))
        {
            outputReady(false);
            if (queue_.closed()) break;
            continue;
        }
        reorder_.add(record);
        outputReady(false);
    }
    //关闭后先强制释放有限窗口中的剩余记录，再刷新两个 Sink 的流缓冲。
    outputReady(true);
    flushSinks();
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
        const SinkResult console_result = console_sink_->write(it->level, line);
        const SinkResult file_result = file_sink_->write(it->level, line);
        if (!console_result.ok) write_errors_.fetch_add(1, std::memory_order_relaxed);
        if (!file_result.ok) write_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void LogAggregator::flushSinks()
{
    //两个 Sink 都尝试 flush；分别累计错误，避免一个 Sink 失败跳过另一个。
    const SinkResult console_result = console_sink_->flush();
    const SinkResult file_result = file_sink_->flush();
    if (!console_result.ok) write_errors_.fetch_add(1, std::memory_order_relaxed);
    if (!file_result.ok) write_errors_.fetch_add(1, std::memory_order_relaxed);
}

} //namespace ipc_log
