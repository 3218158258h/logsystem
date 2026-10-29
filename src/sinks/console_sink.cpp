#include "ipc_log/sink.h"

#include <iostream>

namespace ipc_log {

//汇聚线程持有此 Sink；互斥锁也保护服务端命令回显不与日志行交错。
SinkResult ConsoleSink::write(LogLevel level, const std::string &line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostream &stream = level == LogLevel::Error ?
                           static_cast<std::ostream &>(std::cerr) :
                           static_cast<std::ostream &>(std::cout);
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.flush();
    if (!stream) return SinkResult(false, "console write failed");
    return SinkResult();
}

SinkResult ConsoleSink::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout.flush();
    std::cerr.flush();
    if (!std::cout || !std::cerr) return SinkResult(false, "console flush failed");
    return SinkResult();
}

} //namespace ipc_log
