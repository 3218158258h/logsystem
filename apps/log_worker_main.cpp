#include "app_support.h"

#include "ipc_log/fdbus_transport.h"
#include "ipc_log/logger.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

//worker 装配 Logger/FDBus target，生成演示记录，退出时排空本地发送队列。
int main(int argc, char **argv)
{
    try
    {
        ipc_log_app::Arguments args(argc, argv);
        ipc_log_app::installSignals();
        ipc_log::FdbusTargetConfig target_config;
        target_config.service_name = args.get("service", "ipc_log_service");
        target_config.queue_capacity = static_cast<std::size_t>(args.number("queue", 2048));
        target_config.request_timeout = std::chrono::milliseconds(args.number("request-timeout-ms", 2000));
        target_config.shutdown_timeout = std::chrono::milliseconds(args.number("shutdown-timeout-ms", 10000));

        std::shared_ptr<ipc_log::FdbusLogTarget> target(
            new ipc_log::FdbusLogTarget(target_config));
        ipc_log::LoggerConfig logger_config;
        logger_config.threshold = ipc_log_app::parseLevel(args.get("level", "INFO"));
        std::shared_ptr<ipc_log::Logger> logger(new ipc_log::Logger(target, logger_config));
        if (!target->attachLogger(logger))
            throw std::runtime_error("failed to attach worker logger for remote level control");
        const uint64_t count = args.number("count", 100);
        const uint64_t interval_ms = args.number("interval-ms", 50);
        const std::string worker_name = args.get("name", "worker");
        //每次迭代按当前阈值提交四级样例；动态调级由服务端回调并发修改阈值。
        uint64_t produced = 0;
        while (!ipc_log_app::stopping() && produced < count)
        {
            const std::string module = worker_name + ".orders";
            logger->debug(module, "debug event " + std::to_string(produced));
            logger->info(module, "processed event " + std::to_string(produced));
            if (produced % 5 == 0)
                logger->warn(module, "queue depth is being sampled at event " + std::to_string(produced));
            if (produced % 17 == 0)
                logger->error(module, "synthetic recoverable error " + std::to_string(produced));
            ++produced;
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }

        //shutdown 停止接收后把本地队列发完；服务端退出时统一排空并写出。
        const ipc_log::ShutdownResult result = target->shutdown();
        const ipc_log::TargetStats stats = result.stats;
        std::cerr << worker_name << " shutdown=" << (result.ok() ? "drained" : result.message)
                  << " accepted=" << stats.accepted << " dropped=" << stats.dropped
                  << " rejected=" << stats.rejected << " uncertain=" << stats.uncertain
                  << std::endl;
        return result.ok() ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << "log_worker: " << error.what() << '\n';
        return 1;
    }
}
