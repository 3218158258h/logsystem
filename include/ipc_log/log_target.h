#ifndef IPC_LOG_LOG_TARGET_H
#define IPC_LOG_LOG_TARGET_H

#include "ipc_log/log_record.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace ipc_log {

enum class FlushStatus
{
    Success,
    Partial,
    Timeout,
    WriteError,
    Stopped,
    QueueFull,
    TransportError
};

struct FlushResult
{
    //Partial 表示屏障完成但有损失；uncertain 表示传输超时后无法判定服务端是否处理。
    FlushStatus status;
    uint64_t dropped;
    uint64_t rejected;
    uint64_t uncertain;
    uint64_t write_errors;
    std::string message;

    FlushResult()
        : status(FlushStatus::Success)
        , dropped(0)
        , rejected(0)
        , uncertain(0)
        , write_errors(0)
    {}

    //只有无错误的 Success 才返回 true；Partial 仍需由调用方检查计数和 message。
    bool ok() const { return status == FlushStatus::Success; }
};

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

//Logger 只依赖该接口；submit 立即返回，I/O 和 FDBus 应答由实现的后台路径处理。
class ILogTarget
{
public:
    virtual ~ILogTarget() {}
    //实现必须有界且快速返回；返回 true 是接收入队，不等价于持久化。
    virtual bool submit(LogRecord record) = 0;
    //等待该目标定义的 flush 屏障；超时是否 uncertain 由具体传输语义填充。
    virtual FlushResult flush(std::chrono::milliseconds timeout) = 0;
    //停止接收并完成最后一次 flush；返回值就是这次收尾 flush 的结果。
    virtual FlushResult shutdown(std::chrono::milliseconds timeout) = 0;
    virtual TargetStats stats() const = 0;
};

} //namespace ipc_log

#endif
