#ifndef IPC_LOG_SINK_H
#define IPC_LOG_SINK_H

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

namespace ipc_log {

struct SinkResult
{
    //写入或刷新失败必须带可见原因，由汇聚线程累计到 write_errors。
    bool ok;
    std::string message;
    SinkResult(bool success = true, const std::string &detail = std::string())
        : ok(success), message(detail) {}
};

//最终输出接口由汇聚消费者调用；每个实现负责写入和明确报告刷新错误。
class ILogSink
{
public:
    virtual ~ILogSink() {}
    virtual SinkResult write(const std::string &line) = 0;
    virtual SinkResult flush() = 0;
};

//控制台 Sink 将 ERROR 写入 stderr，其余等级写入 stdout。
class ConsoleSink : public ILogSink
{
public:
    SinkResult write(const std::string &line);
    SinkResult flush();
private:
    std::mutex mutex_;
};

struct RotatingFileConfig
{
    std::string path;
    std::size_t max_bytes;
    std::size_t backup_count;
    //主文件加 backup_count 个编号备份；max_bytes 以格式化后的字节数计算。
    RotatingFileConfig() : path("logs/ipc-log.log"), max_bytes(5 * 1024 * 1024), backup_count(3) {}
};

//文件 Sink 按字节数切割，并限制备份数量以保持磁盘占用有界。
class RotatingFileSink : public ILogSink
{
public:
    explicit RotatingFileSink(const RotatingFileConfig &config);
    ~RotatingFileSink();
    SinkResult write(const std::string &line);
    SinkResult flush();

private:
    SinkResult openFile();
    SinkResult rotate();
    std::string backupPath(std::size_t index) const;

    RotatingFileConfig config_;
    std::ofstream stream_;
    std::size_t current_bytes_;
    std::mutex mutex_;
};

} //namespace ipc_log

#endif
