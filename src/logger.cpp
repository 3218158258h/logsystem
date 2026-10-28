#include "ipc_log/logger.h"

#include <algorithm>

namespace ipc_log {

//Logger 在业务线程构造轻量记录并调用目标入队；输出线程归目标所有。
Logger::Logger(const std::shared_ptr<ILogTarget> &target, const LoggerConfig &config)
    : target_(target)
    , threshold_(static_cast<uint32_t>(config.threshold))
    , filtered_(0), truncated_(0), submitted_(0), rejected_(0)
    , pid_(currentProcessId()), process_instance_(createProcessInstanceId(pid_))
{
}

void Logger::setLevel(LogLevel level)
{
    //阈值是原子快照，正在并发 log 的线程立即按新旧任一完整等级判断。
    threshold_.store(static_cast<uint32_t>(level), std::memory_order_relaxed);
}

LogLevel Logger::level() const
{
    return static_cast<LogLevel>(threshold_.load(std::memory_order_relaxed));
}

bool Logger::log(LogLevel log_level, const std::string &module,
                 const std::string &message)
{
    //先做等级过滤，关闭的等级不会构造记录或触达目标队列。
    if (static_cast<uint32_t>(log_level) < threshold_.load(std::memory_order_relaxed))
    {
        filtered_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!target_)
    {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    //截断按字节计数，保证序列化字段有界；时间戳和正文在调用时形成不可变快照。
    LogRecord record;
    record.timestamp_ms = currentTimeMillis();
    record.level = log_level;
    record.pid = pid_;
    record.process_instance = process_instance_;
    record.module.assign(module.data(), std::min(module.size(), kMaxModuleBytes));
    record.message.assign(message.data(), std::min(message.size(), kMaxMessageBytes));
    if (record.module.size() != module.size() || record.message.size() != message.size())
        truncated_.fetch_add(1, std::memory_order_relaxed);

    //目标只尝试有界入队，不等待磁盘写入或 FDBus 应答。
    if (target_->submit(record))
    {
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool Logger::debug(const std::string &module, const std::string &message)
{ return log(LogLevel::Debug, module, message); }
bool Logger::info(const std::string &module, const std::string &message)
{ return log(LogLevel::Info, module, message); }
bool Logger::warn(const std::string &module, const std::string &message)
{ return log(LogLevel::Warn, module, message); }
bool Logger::error(const std::string &module, const std::string &message)
{ return log(LogLevel::Error, module, message); }

LoggerStats Logger::stats() const
{
    //统计项是各业务线程可并发更新的累计值，不代表某个原子时刻的一致快照。
    LoggerStats result;
    result.filtered = filtered_.load(std::memory_order_relaxed);
    result.truncated = truncated_.load(std::memory_order_relaxed);
    result.submitted = submitted_.load(std::memory_order_relaxed);
    result.rejected = rejected_.load(std::memory_order_relaxed);
    return result;
}

} //namespace ipc_log
