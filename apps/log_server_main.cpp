#include "app_support.h"

#include "ipc_log/fdbus_transport.h"
#include "ipc_log/log_aggregator.h"
#include "ipc_log/logger.h"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void printCommandLine(ipc_log::ConsoleSink &console, const std::string &line)
{
    //复用汇聚线程持有的 ConsoleSink，避免命令回显和日志输出互相穿插。
    const ipc_log::SinkResult result = console.write(ipc_log::LogLevel::Info, line + "\n");
    if (!result.ok) std::cerr << "log_server console output failed: " << result.message << std::endl;
}

bool parseInstanceId(const std::string &text, uint64_t &value)
{
    return ipc_log_app::parseUnsignedDecimal(text, value) && value != 0;
}

void printWorker(ipc_log::ConsoleSink &console, const ipc_log::WorkerInfo &worker)
{
    std::ostringstream output;
    output << "control worker session=" << worker.session_id << " pid=";
    if (worker.pid) output << worker.pid;
    else output << "unknown";
    output << " process_instance=";
    if (worker.process_instance) output << worker.process_instance;
    else output << "unknown";
    output << " level=" << (worker.level_known ? ipc_log::logLevelName(worker.level) : "unknown")
           << " status=" << worker.status;
    printCommandLine(console, output.str());
}

void printControlResult(ipc_log::ConsoleSink &console, const std::string &command, uint64_t instance,
                        const ipc_log::WorkerControlResult &result)
{
    std::ostringstream output;
    output << "control " << command << " process_instance=" << instance
           << " confirmed=" << (result.confirmed ? "true" : "false")
           << " uncertain=" << (result.uncertain ? "true" : "false")
           << " pid=";
    if (result.worker.pid) output << result.worker.pid;
    else output << "unknown";
    output << " level=" << (result.worker.level_known ?
                             ipc_log::logLevelName(result.worker.level) : "unknown")
           << " status=" << result.message;
    printCommandLine(console, output.str());
}

void handleCommand(const std::string &line, ipc_log::LogServer &server,
                   ipc_log::ConsoleSink &console, std::chrono::milliseconds timeout)
{
    //命令线程只解析三种控制操作；等待有超时，不进入 FDBus context 回调。
    std::istringstream input(line);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) words.push_back(word);
    if (words.empty()) return;
    if (words.size() == 1 && words[0] == "list")
    {
        const std::vector<ipc_log::WorkerInfo> workers = server.listWorkers(timeout);
        if (workers.empty()) printCommandLine(console, "control list: no online workers");
        for (std::vector<ipc_log::WorkerInfo>::const_iterator it = workers.begin();
             it != workers.end(); ++it)
            printWorker(console, *it);
        return;
    }
    if ((words.size() == 2 && words[0] == "get-level") ||
        (words.size() == 3 && words[0] == "set-level"))
    {
        uint64_t instance = 0;
        if (!parseInstanceId(words[1], instance))
        {
            printCommandLine(console, "control error: process_instance must be a positive decimal integer");
            return;
        }
        if (words[0] == "get-level")
        {
            printControlResult(console, "get-level", instance,
                               server.getWorkerLevel(instance, timeout));
            return;
        }
        try
        {
            const ipc_log::LogLevel level = ipc_log_app::parseLevel(words[2]);
            printControlResult(console, "set-level", instance,
                               server.setWorkerLevel(instance, level, timeout));
        }
        catch (const std::invalid_argument &error)
        {
            printCommandLine(console, std::string("control error: ") + error.what());
        }
        return;
    }
    printCommandLine(console, "control error: use list | get-level <process_instance> | "
                     "set-level <process_instance> <DEBUG|INFO|WARN|ERROR>");
}

void readCommands(ipc_log::LogServer &server, ipc_log::ConsoleSink &console,
                  std::chrono::milliseconds timeout)
{
    //poll/read 保持服务进程可处理信号；长行丢弃，EOF 只关闭命令输入。
    const std::size_t max_line_bytes = 256;
    std::string line;
    bool input_open = true;
    bool discarding = false;
    while (!ipc_log_app::stopping())
    {
        struct pollfd descriptor;
        descriptor.fd = STDIN_FILENO;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        const int ready = ::poll(input_open ? &descriptor : 0,
                                 input_open ? 1 : 0, 100);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("poll stdin failed: ") + std::strerror(errno));
        }
        if (!input_open || ready == 0) continue;
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0)
        {
            if (descriptor.revents & (POLLERR | POLLNVAL))
            {
                input_open = false;
                printCommandLine(console, "log_server command input disabled after poll error");
            }
            continue;
        }

        char buffer[128];
        const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count == 0)
        {
            input_open = false;
            if (discarding)
                printCommandLine(console, "control error: oversized command discarded at stdin EOF");
            else if (!line.empty())
                printCommandLine(console, "control error: incomplete command discarded at stdin EOF");
            line.clear();
            printCommandLine(console, "log_server command input closed; service remains active");
            continue;
        }
        if (count < 0)
        {
            if (errno == EINTR || errno == EAGAIN) continue;
            input_open = false;
            printCommandLine(console, std::string("log_server command input disabled: ") +
                             std::strerror(errno));
            continue;
        }
        for (ssize_t i = 0; i < count; ++i)
        {
            const char ch = buffer[i];
            if (ch == '\n')
            {
                if (discarding)
                    printCommandLine(console, std::string("control error: command exceeds ") +
                                     std::to_string(max_line_bytes) + " bytes and was discarded");
                else
                {
                    if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
                    handleCommand(line, server, console, timeout);
                }
                line.clear();
                discarding = false;
            }
            else if (!discarding)
            {
                if (line.size() == max_line_bytes)
                {
                    line.clear();
                    discarding = true;
                }
                else
                    line.push_back(ch);
            }
        }
    }
}

} //namespace

int main(int argc, char **argv)
{
    try
    {
        //入口负责装配共享 Sink、汇聚器、服务端和本地控制 Logger，再启动 service。
        //处理传入的参数
        ipc_log_app::Arguments args(argc, argv);
        ipc_log_app::installSignals();
        //配置FDBus
        ipc_log::LogServerConfig server_config;
        server_config.service_name = args.get("service", "ipc_log_service");
        //配置汇聚器
        ipc_log::AggregatorConfig aggregator_config;
        aggregator_config.queue_capacity = static_cast<std::size_t>(args.number("queue", 8192));
        //配置重排缓冲区
        aggregator_config.reorder.capacity = static_cast<std::size_t>(args.number("reorder-capacity", 4096));
        aggregator_config.reorder.window_ms = args.number("window-ms", 50);
        aggregator_config.reorder.idle_release =
            std::chrono::milliseconds(args.number("idle-release-ms", 100));
        if (aggregator_config.reorder.capacity > 65536 ||
            aggregator_config.reorder.idle_release.count() > 60000)
            throw std::invalid_argument("reorder capacity must be <=65536 and idle release <=60000ms");
        //创建控制台输出端、日志文件输出端，并接入到汇聚器
        std::shared_ptr<ipc_log::ConsoleSink> console_sink(new ipc_log::ConsoleSink());
        ipc_log::LogAggregator aggregator(console_sink,
            ipc_log_app::makeFileSink(args, "logs/aggregated.log"), aggregator_config);
        //将日志系统的输出日志接入到汇聚器
        const std::shared_ptr<ipc_log::ILogTarget> aggregator_target(
            &aggregator, [](ipc_log::ILogTarget *) {});
        ipc_log::LoggerConfig logger_config;
        logger_config.threshold = ipc_log_app::parseLevel(args.get("level", "INFO"));
        ipc_log::Logger logger(aggregator_target, logger_config);
        ipc_log::LogServer server(aggregator, server_config);
        if (!server.start())
            throw std::runtime_error("failed to bind FDBus log service");
        logger.info("log_server", "service bound at svc://" + server_config.service_name);
        printCommandLine(*console_sink,
                         "log_server listening at svc://" + server_config.service_name);
        const std::chrono::milliseconds control_timeout(args.number("control-timeout-ms", 2000));
        if (control_timeout.count() <= 0 || control_timeout.count() > 60000)
            throw std::invalid_argument("control timeout must be in 1..60000ms");
        //命令 EOF 仅关闭 stdin 控制面；SIGINT/SIGTERM 才结束服务生命周期。
        readCommands(server, *console_sink, control_timeout);

        logger.info("log_server", "shutdown requested; draining worker and output queues");
        //先停止接收，再关闭汇聚队列；消费线程会把剩余记录写完并刷新 Sink。
        server.shutdown(std::chrono::seconds(10));
        logger.info("log_server", "FDBus endpoint stopped");
        const ipc_log::ShutdownResult result = aggregator.shutdown();
        const ipc_log::TargetStats stats = result.stats;
        std::ostringstream summary;
        summary << "log_server stopped accepted=" << stats.accepted
                << " dropped=" << stats.dropped << " rejected=" << stats.rejected
                << " write_errors=" << stats.write_errors
                << " late=" << stats.late_records
                << " forced_releases=" << stats.forced_releases;
        printCommandLine(*console_sink, summary.str());
        if (!result.ok())
            printCommandLine(*console_sink, "log_server shutdown: " + result.message);
        return result.ok() ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << "log_server: " << error.what() << '\n';
        return 1;
    }
}
