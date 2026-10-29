#include "ipc_log/reorder_buffer.h"

#include <stdexcept>

namespace ipc_log {

//只在汇聚消费线程调用；排序窗口按日志墙钟，空闲期限按单调时钟。
ReorderBuffer::ReorderBuffer(const ReorderConfig &config)
    : config_(config), max_seen_timestamp_(0), last_released_timestamp_(0)
    , late_records_(0), forced_releases_(0)
    , last_arrival_(std::chrono::steady_clock::now())
{
    if (config_.capacity == 0 || config_.capacity > 65536 ||
        config_.idle_release.count() <= 0 || config_.idle_release.count() > 60000)
        throw std::invalid_argument("reorder capacity must be 1..65536 and idle release 1..60000ms");
}

//比较消息
bool ReorderBuffer::Less::operator()(const LogRecord &left, const LogRecord &right) const
{
    if (left.timestamp_ms != right.timestamp_ms) return left.timestamp_ms < right.timestamp_ms;
    //同毫秒记录用稳定字段打破平局；这只保证确定性，不代表跨进程因果顺序。
    if (left.pid != right.pid) return left.pid < right.pid;
    if (left.process_instance != right.process_instance)
        return left.process_instance < right.process_instance;
    if (left.sequence != right.sequence) return left.sequence < right.sequence;
    if (left.module != right.module) return left.module < right.module;
    return left.message < right.message;
}
//插入重排缓冲区
void ReorderBuffer::add(const LogRecord &record)
{
    //已释放水位之后到达的记录计为 late，但仍照常输出，不静默丢弃。
    if (record.timestamp_ms < last_released_timestamp_) ++late_records_;
    records_.insert(record);
    if (record.timestamp_ms > max_seen_timestamp_) max_seen_timestamp_ = record.timestamp_ms;
    last_arrival_ = std::chrono::steady_clock::now();
}

//尝试输出
void ReorderBuffer::releaseReady(std::chrono::steady_clock::time_point now,
                                 bool force, std::vector<LogRecord> &released)
{
    //时间水位限制重排范围；
    const bool idle = !records_.empty() && now - last_arrival_ >= config_.idle_release;
    const bool drain = force || idle;
    const uint64_t watermark = max_seen_timestamp_ > config_.window_ms ?
                               max_seen_timestamp_ - config_.window_ms : 0;
    //循环输出
    while (!records_.empty())
    {
        const bool over_capacity = records_.size() > config_.capacity;
        const LogRecord &head = *records_.begin();
        if (!drain && !over_capacity && head.timestamp_ms > watermark) break;
        if (over_capacity && !drain) ++forced_releases_;
        released.push_back(head);
        if (head.timestamp_ms > last_released_timestamp_)
            last_released_timestamp_ = head.timestamp_ms;
        records_.erase(records_.begin());
    }
}

uint64_t ReorderBuffer::lateRecords() const { return late_records_.load(std::memory_order_relaxed); }
uint64_t ReorderBuffer::forcedReleases() const { return forced_releases_.load(std::memory_order_relaxed); }

} //namespace ipc_log
