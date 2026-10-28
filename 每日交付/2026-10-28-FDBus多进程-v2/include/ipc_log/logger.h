#ifndef IPC_LOG_LOGGER_H
#define IPC_LOG_LOGGER_H

#include "ipc_log/log_target.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ipc_log {

struct LoggerConfig//logger配置
{
    LogLevel threshold;
    LoggerConfig() : threshold(LogLevel::Info) {}
};

struct LoggerStats//worker端统计计数
{
    uint64_t filtered;
    uint64_t truncated;
    uint64_t submitted;
    uint64_t rejected;
    LoggerStats() : filtered(0), truncated(0), submitted(0), rejected(0) {}
};

//业务线程入口：先按进程阈值过滤，再构造记录并尝试提交到目标的有界队列。
class Logger
{
public:
    explicit Logger(const std::shared_ptr<ILogTarget> &target,
                    const LoggerConfig &config = LoggerConfig());

    //运行时调整此进程阈值
    void setLevel(LogLevel level);
    LogLevel level() const;
    //返回 true 表示可以入队
    bool log(LogLevel level, const std::string &module, const std::string &message);
    bool debug(const std::string &module, const std::string &message);
    bool info(const std::string &module, const std::string &message);
    bool warn(const std::string &module, const std::string &message);
    bool error(const std::string &module, const std::string &message);
    LoggerStats stats() const;

private:
    std::shared_ptr<ILogTarget> target_;
    std::atomic<uint32_t> threshold_;
    std::atomic<uint64_t> filtered_;
    std::atomic<uint64_t> truncated_;
    std::atomic<uint64_t> submitted_;
    std::atomic<uint64_t> rejected_;
    const uint32_t pid_;
    const uint64_t process_instance_;
};

} //namespace ipc_log

#endif
