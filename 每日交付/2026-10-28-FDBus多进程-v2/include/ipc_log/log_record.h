#ifndef IPC_LOG_LOG_RECORD_H
#define IPC_LOG_LOG_RECORD_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace ipc_log {

//枚举值与 Protobuf 等级字段一致，阈值比较按 DEBUG 到 ERROR 递增。
enum class LogLevel : uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

//Logger 截断与服务端 wire 校验共用的字段字节上限。
constexpr std::size_t kMaxModuleBytes = 128;
constexpr std::size_t kMaxMessageBytes = 4096;

const char *logLevelName(LogLevel level);//获取过滤等级
bool isValidLogLevel(uint32_t level);//判断等级是否合法
uint64_t currentTimeMillis();//获取系统时间
uint32_t currentProcessId();//获取进程pid
uint64_t createProcessInstanceId(uint32_t pid);//获取实例号

//Logger 过滤后构造此记录；传给目标前完成身份、时间和正文快照。
struct LogRecord
{
    uint64_t timestamp_ms;//时间戳
    LogLevel level;//消息等级
    uint32_t pid;//进程pid
    std::string module;//模块
    std::string message;//正文
    uint64_t sequence;//序号
    uint64_t process_instance;//实例号

    LogRecord()
        : timestamp_ms(0)
        , level(LogLevel::Info)
        , pid(0)
        , sequence(0)
        , process_instance(0)
    {}
};

} //namespace ipc_log

#endif