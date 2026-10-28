#ifndef IPC_LOG_APP_SUPPORT_H
#define IPC_LOG_APP_SUPPORT_H

#include "ipc_log/log_record.h"
#include "ipc_log/sink.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <signal.h>
#include <string>
#include <stdexcept>

namespace ipc_log_app {

//共用命令行、信号和 Sink 装配；应用特有的生命周期仍留在各自 main 中。
static volatile std::sig_atomic_t stop_requested = 0;

//信号处理只置位，线程和资源清理留给主循环执行。
inline void stopSignalHandler(int) { stop_requested = 1; }

inline void installSignals()
{
    std::signal(SIGINT, stopSignalHandler);
    std::signal(SIGTERM, stopSignalHandler);
}

class Arguments
{
public:
    //解析 --key value 和无值 --flag；不认识的键保留，由调用方决定是否使用。
    Arguments(int argc, char **argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string key(argv[i]);
            if (key.compare(0, 2, "--") != 0)//不是--key类型则跳过
                continue;
            if (i + 1 < argc && std::string(argv[i + 1]).compare(0, 2, "--") != 0)
                values_[key.substr(2)] = argv[++i];
            else
                values_[key.substr(2)] = "true";//bool型选项，直接启动
        }
    }
    //获取参数，未找到返回默认值
    std::string get(const std::string &key, const std::string &fallback) const
    {
        std::map<std::string, std::string>::const_iterator it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }
    //读取参数，未读到返回默认
    uint64_t number(const std::string &key, uint64_t fallback) const
    {
        //数值参数必须完整解析为十进制，防止尾随字符被静默忽略。
        const std::string value = get(key, std::string());
        if (value.empty()) return fallback;
        char *end = 0;
        errno = 0;
        const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
        if (errno || !end || *end != '\0')
            throw std::invalid_argument("invalid numeric argument --" + key + ": " + value);
        return static_cast<uint64_t>(parsed);
    }

private:
    std::map<std::string, std::string> values_;
};

inline ipc_log::LogLevel parseLevel(const std::string &text)
{
    //演示入口接受大小写两种拼法，不接受缩写或隐式默认等级。
    if (text == "DEBUG" || text == "debug") return ipc_log::LogLevel::Debug;
    if (text == "INFO" || text == "info") return ipc_log::LogLevel::Info;
    if (text == "WARN" || text == "warn") return ipc_log::LogLevel::Warn;
    if (text == "ERROR" || text == "error") return ipc_log::LogLevel::Error;
    throw std::invalid_argument("level must be DEBUG, INFO, WARN, or ERROR");
}

inline ipc_log::RotatingFileConfig fileConfig(const Arguments &args,
                                               const std::string &default_path)
{
    //文件路径、切割阈值和备份数共用同一组演示参数。
    ipc_log::RotatingFileConfig config;
    config.path = args.get("file", default_path);
    config.max_bytes = static_cast<std::size_t>(args.number("max-bytes", config.max_bytes));
    config.backup_count = static_cast<std::size_t>(args.number("backups", config.backup_count));
    return config;
}

inline std::shared_ptr<ipc_log::ILogSink> makeConsoleSink()
{
    return std::shared_ptr<ipc_log::ILogSink>(new ipc_log::ConsoleSink());
}

inline std::shared_ptr<ipc_log::ILogSink> makeFileSink(const Arguments &args,
                                                        const std::string &default_path)
{
    return std::shared_ptr<ipc_log::ILogSink>(
        new ipc_log::RotatingFileSink(fileConfig(args, default_path)));
}

inline bool stopping() { return stop_requested != 0; }

} //namespace ipc_log_app

#endif
