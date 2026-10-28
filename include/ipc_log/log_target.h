#ifndef IPC_LOG_LOG_TARGET_H
#define IPC_LOG_LOG_TARGET_H

#include "ipc_log/log_record.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace ipc_log {

struct TargetStats//队列统计结果，可以是业务进程的本地队列  也可以是服务端的总队列
{
    uint64_t accepted;//成功入队的
    uint64_t dropped;//队列满/入队但未发送的
    uint64_t rejected;//发出后被拒绝的
    uint64_t uncertain;
    uint64_t write_errors;//输出时错误的
    uint64_t late_records;//重排超时的
    uint64_t forced_releases;//排序满/退出导致强制释放的

    TargetStats()
        : accepted(0), dropped(0), rejected(0), uncertain(0), write_errors(0)
        , late_records(0), forced_releases(0)
    {}
};

//一次Shutdown的结果：是否在时限内排空本地队列，以及排空前后的累计计数。
//没有远程 flush 协议；drained 只描述本目标自己的队列和输出线程。
struct ShutdownResult
{
    bool drained;//本次收尾是否在实现自己的时限内完成了排空
    bool already_stopped;//之前已经关闭过，本次没有再执行排空
    TargetStats stats;
    std::string message;//非正常收尾时的原因说明；正常时通常为空

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
