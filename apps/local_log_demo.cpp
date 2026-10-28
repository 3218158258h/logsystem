#include "app_support.h"

#include "ipc_log/local_log_target.h"
#include "ipc_log/logger.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

//本地演示绕过 FDBus，复用同一 Logger、汇聚队列、重排器和两个 Sink。
int main(int argc, char **argv)
{
    try
    {
        ipc_log_app::Arguments args(argc, argv);
        ipc_log::AggregatorConfig aggregate_config;
        aggregate_config.queue_capacity = static_cast<std::size_t>(args.number("queue", 8192));

        std::shared_ptr<ipc_log::LocalLogTarget> target(new ipc_log::LocalLogTarget(
            ipc_log_app::makeConsoleSink(), ipc_log_app::makeFileSink(args, "logs/local-demo.log"),
            aggregate_config));
        ipc_log::LoggerConfig logger_config;
        logger_config.threshold = ipc_log_app::parseLevel(args.get("level", "INFO"));
        ipc_log::Logger logger(target, logger_config);

        const uint64_t count = args.number("count", 20);
        const uint64_t producers = args.number("producers", 1);
        if (producers == 0 || producers > 64)
            throw std::invalid_argument("--producers must be between 1 and 64");
        //多个生产线程只调用 Logger；输出由 aggregator 的后台消费者串行完成。
        std::vector<std::thread> threads;
        for (uint64_t producer = 0; producer < producers; ++producer)
        {
            threads.push_back(std::thread([&logger, count, producer]()
            {
                for (uint64_t i = 0; i < count; ++i)
                {
                    const std::string module = "local-worker-" + std::to_string(producer);
                    const std::string text = "local demo record " + std::to_string(i);
                    logger.debug(module, text + " [debug]");
                    logger.info(module, text + " [info]");
                    logger.warn(module, text + " [warn]");
                    logger.error(module, text + " [error]");
                }
            }));
        }
        for (std::vector<std::thread>::iterator it = threads.begin(); it != threads.end(); ++it)
            it->join();

        //等级过滤发生在记录入队前；shutdown 在停止接收后等待后台汇聚线程完成输出。
        logger.setLevel(ipc_log::LogLevel::Debug);
        logger.debug("local-demo", "runtime threshold changed to DEBUG");
        const ipc_log::FlushResult flushed = target->shutdown(std::chrono::seconds(10));
        const ipc_log::TargetStats stats = target->stats();
        std::cout << "local demo flush=" << (flushed.ok() ? "ok" : flushed.message)
                  << " accepted=" << stats.accepted << " dropped=" << stats.dropped
                  << " write_errors=" << stats.write_errors << '\n';
        return flushed.ok() ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << "local_log_demo: " << error.what() << '\n';
        return 1;
    }
}
