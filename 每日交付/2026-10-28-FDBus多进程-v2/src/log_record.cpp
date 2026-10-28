#include "ipc_log/log_record.h"

#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ipc_log {

//获取过滤等级
//协议等级使用连续整数，因此过滤阈值与 wire 值共用同一排序。
const char *logLevelName(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INVALID";
}

//判断等级是否合法
bool isValidLogLevel(uint32_t level)
{
    return level <= static_cast<uint32_t>(LogLevel::Error);
}

//获取系统时间
uint64_t currentTimeMillis()
{
    //记录排序使用墙钟毫秒；重排器另用 steady_clock 控制等待时长。
    const std::chrono::system_clock::duration elapsed =
        std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

//获取进程id
uint32_t currentProcessId()
{
#ifdef _WIN32
    return static_cast<uint32_t>(_getpid());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

//获取实例号
uint64_t createProcessInstanceId(uint32_t pid)
{
    //实例号用于区分 PID 复用；它是会话内身份标识，不承诺密码学唯一性。
    static std::atomic<uint64_t> nonce(0);
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint64_t value = ticks ^ (static_cast<uint64_t>(pid) << 32) ^
                     (++nonce * 0x9e3779b97f4a7c15ULL);
    return value ? value : 1;
}

} //namespace ipc_log
