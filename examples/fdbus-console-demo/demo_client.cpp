//控制台日志示例：持续读取用户输入，按固定格式调用日志库，并接受服务端集中调级。
//本文件只依赖 ipc_log 的公开头文件，不包含日志系统内部实现或 apps/ 下的辅助代码。
#include "ipc_log/fdbus_transport.h"
#include "ipc_log/log_record.h"
#include "ipc_log/logger.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

struct Options
{
    std::string name;
    std::string service;
    ipc_log::LogLevel level;
    std::size_t queue_capacity;
    std::chrono::milliseconds request_timeout;
    std::chrono::milliseconds shutdown_timeout;

    Options()
        : name("demo"), service("ipc_log_service"), level(ipc_log::LogLevel::Info)
        , queue_capacity(2048), request_timeout(2000), shutdown_timeout(10000) {}
};

uint64_t parseUnsigned(const std::string &text, const std::string &key)
{
    //示例参数同样拒绝负号和尾随字符，避免把错误输入当成巨大配置值。
    if (text.empty() || text[0] == '-') throw std::invalid_argument("invalid " + key + ": " + text);
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed);
    if (consumed != text.size()) throw std::invalid_argument("invalid " + key + ": " + text);
    return static_cast<uint64_t>(value);
}

ipc_log::LogLevel parseLevel(const std::string &text)
{
    std::string upper(text);
    for (std::size_t i = 0; i < upper.size(); ++i)
        upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    if (upper == "DEBUG") return ipc_log::LogLevel::Debug;
    if (upper == "INFO") return ipc_log::LogLevel::Info;
    if (upper == "WARN") return ipc_log::LogLevel::Warn;
    if (upper == "ERROR") return ipc_log::LogLevel::Error;
    throw std::invalid_argument("level must be DEBUG, INFO, WARN, or ERROR");
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string key(argv[i]);
        if (key == "--name" && i + 1 < argc)
            options.name = argv[++i];
        else if (key == "--service" && i + 1 < argc)
            options.service = argv[++i];
        else if (key == "--level" && i + 1 < argc)
            options.level = parseLevel(argv[++i]);
        else if (key == "--queue" && i + 1 < argc)
            options.queue_capacity = static_cast<std::size_t>(parseUnsigned(argv[++i], key));
        else if (key == "--request-timeout-ms" && i + 1 < argc)
            options.request_timeout = std::chrono::milliseconds(parseUnsigned(argv[++i], key));
        else if (key == "--shutdown-timeout-ms" && i + 1 < argc)
            options.shutdown_timeout = std::chrono::milliseconds(parseUnsigned(argv[++i], key));
        else
            throw std::invalid_argument("unknown option or missing value: " + key);
    }
    if (options.name.empty()) throw std::invalid_argument("--name must not be empty");
    return options;
}

void printUsage()
{
    std::cout << "input: LEVEL MODULE MESSAGE      e.g. INFO orders hello world\n"
              << "local: :level DEBUG | :stats | :help | :quit\n";
}

void printStats(const ipc_log::Logger &logger)
{
    const ipc_log::LoggerStats stats = logger.stats();
    std::cout << "logger filtered=" << stats.filtered
              << " truncated=" << stats.truncated
              << " submitted=" << stats.submitted
              << " rejected=" << stats.rejected << '\n';
}

} //namespace

int main(int argc, char **argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);

        ipc_log::FdbusTargetConfig target_config;
        target_config.service_name = options.service;
        target_config.queue_capacity = options.queue_capacity;
        target_config.request_timeout = options.request_timeout;
        target_config.shutdown_timeout = options.shutdown_timeout;

        std::shared_ptr<ipc_log::FdbusLogTarget> target(
            new ipc_log::FdbusLogTarget(target_config));
        ipc_log::LoggerConfig logger_config;
        logger_config.threshold = options.level;
        std::shared_ptr<ipc_log::Logger> logger(
            new ipc_log::Logger(target, logger_config));
        if (!target->attachLogger(logger))
            throw std::runtime_error("failed to attach logger for remote level control");

        std::cout << options.name << " ready"
                  << " pid=" << ipc_log::currentProcessId()
                  << " instance=" << target->processInstance()
                  << " service=svc://" << options.service << '\n';
        printUsage();

        //主线程持续读终端；每条日志只做一次入队，发送由目标的后台线程完成。
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line.empty()) continue;
            if (line[0] == ':')
            {
                std::istringstream command(line.substr(1));
                std::string name;
                command >> name;
                if (name == "quit")
                    break;
                if (name == "help")
                {
                    printUsage();
                    continue;
                }
                if (name == "stats")
                {
                    printStats(*logger);
                    continue;
                }
                if (name == "level")
                {
                    std::string level_text;
                    command >> level_text;
                    try
                    {
                        logger->setLevel(parseLevel(level_text));
                        std::cout << "local level=" << ipc_log::logLevelName(logger->level()) << '\n';
                    }
                    catch (const std::exception &error)
                    {
                        std::cout << "level error: " << error.what() << '\n';
                    }
                    continue;
                }
                std::cout << "unknown command: " << line << '\n';
                printUsage();
                continue;
            }

            std::istringstream input(line);
            std::string level_text;
            std::string module;
            if (!(input >> level_text >> module))
            {
                std::cout << "format error: expected LEVEL MODULE MESSAGE\n";
                continue;
            }
            std::string message;
            std::getline(input, message);
            if (!message.empty() && message[0] == ' ') message.erase(0, 1);
            if (message.empty())
            {
                std::cout << "format error: message must not be empty\n";
                continue;
            }

            ipc_log::LogLevel level;
            try
            {
                level = parseLevel(level_text);
            }
            catch (const std::exception &error)
            {
                std::cout << error.what() << '\n';
                continue;
            }
            if (!logger->log(level, module, message))
                std::cout << "not accepted: filtered or queue full\n";
        }

        //EOF 或 :quit 都走同一次收尾；本地队列发完后进程退出。
        const ipc_log::ShutdownResult result = target->shutdown();
        const ipc_log::TargetStats stats = result.stats;
        std::cout << options.name << " shutdown=" << (result.ok() ? "drained" : result.message)
                  << " accepted=" << stats.accepted
                  << " dropped=" << stats.dropped
                  << " rejected=" << stats.rejected
                  << " uncertain=" << stats.uncertain << '\n';
        return result.ok() ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << "demo_client: " << error.what() << '\n';
        return 1;
    }
}
