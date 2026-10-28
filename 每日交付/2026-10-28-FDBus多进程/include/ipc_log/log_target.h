#ifndef IPC_LOG_LOG_TARGET_H
#define IPC_LOG_LOG_TARGET_H

#include "ipc_log/log_record.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace ipc_log {

struct TargetStats
{
    uint64_t accepted;
    uint64_t dropped;
    uint64_t rejected;
    uint64_t uncertain;
    uint64_t write_errors;
    uint64_t late_records;
    uint64_t forced_releases;

    TargetStats()
        : accepted(0), dropped(0), rejected(0), uncertain(0), write_errors(0)
        , late_records(0), forced_releases(0)
    {}
};

//一次收尾的结果：是否在时限内排空本地队列，以及排空前后的累计计数。
//没有远程 flush 协议；drained 只描述本目标自己的队列和输出线程。
struct ShutdownResult
{
    bool drained;
    bool already_stopped;
    TargetStats stats;
    std::string message;

    ShutdownResult() : drained(true), already_stopped(false) {}

    //有丢弃、拒绝、结果未知或写失败时不视为正常收尾。
    bool ok() const
    {
        return drained && !already_stopped && stats.dropped == 0 &&
               stats.rejected == 0 && stats.uncertain == 0 &&
               stats.write_errors == 0;
    }
};

//Logger 只依赖该接口；submit 立即返回，I/O 和远端发送由实现的后台路径处理。
class ILogTarget
{
public:
    virtual ~ILogTarget() {}
    //实现必须有界且快速返回；返回 true 是接收入队，不等价于持久化。
    virtual bool submit(LogRecord record) = 0;
    //停止接收并排空本目标已接收的记录；输出由各实现自己的消费/发送路径完成。
    virtual ShutdownResult shutdown() = 0;
    virtual TargetStats stats() const = 0;
};

} //namespace ipc_log

#endif
