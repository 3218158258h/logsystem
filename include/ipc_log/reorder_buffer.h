#ifndef IPC_LOG_REORDER_BUFFER_H
#define IPC_LOG_REORDER_BUFFER_H

#include "ipc_log/log_record.h"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace ipc_log {

struct ReorderConfig
{
    std::size_t capacity;
    uint64_t window_ms;
    std::chrono::milliseconds idle_release;
    //超过 window_ms 的水位可释放；idle_release 和 capacity 保证低流量/过载时有界收敛。
    ReorderConfig() : capacity(4096), window_ms(50), idle_release(100) {}
};

//只在 LogAggregator 消费线程使用；有限时间窗容忍跨进程轻微乱序。
class ReorderBuffer
{
public:
    explicit ReorderBuffer(const ReorderConfig &config);
    void add(const LogRecord &record);
    //把达到水位、空闲时限或强制条件的最早记录追加到 released。
    void releaseReady(std::chrono::steady_clock::time_point now,
                      bool force, std::vector<LogRecord> &released);
    uint64_t lateRecords() const;
    uint64_t forcedReleases() const;

private:
    struct Less
    {
        bool operator()(const LogRecord &left, const LogRecord &right) const;
    };
    ReorderConfig config_;
    std::multiset<LogRecord, Less> records_;
    uint64_t max_seen_timestamp_;
    uint64_t last_released_timestamp_;
    std::atomic<uint64_t> late_records_;
    std::atomic<uint64_t> forced_releases_;
    std::chrono::steady_clock::time_point last_arrival_;
};

} //namespace ipc_log

#endif
