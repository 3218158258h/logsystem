#include "ipc_log/sink.h"

#include <iostream>

namespace ipc_log {

//汇聚线程持有此 Sink；互斥锁也保护服务端命令回显不与日志行交错。
SinkResult ConsoleSink::write(const std::string &line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string::size_type first_close = line.find(']');
    const bool is_error = first_close != std::string::npos &&
                          line.compare(first_close + 1, 7, "[ERROR]") == 0;
    std::ostream &stream = is_error ?
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
