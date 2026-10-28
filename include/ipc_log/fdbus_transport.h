#ifndef IPC_LOG_FDBUS_TRANSPORT_H
#define IPC_LOG_FDBUS_TRANSPORT_H

#include "ipc_log/log_target.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ipc_log {

//客户端与服务端共用的 FDBus 消息编码，两端必须保持一致。
constexpr int32_t kLogRequestCode = 0x4c470101;
constexpr int32_t kLevelControlCode = 0x4c470102;

namespace detail {
//进程内共享 FDBus 全局 context；首个使用者启动，最后一个使用者销毁。
//FDBus 5.4 的 destroy() 不会复位单例指针，因此销毁后本进程不能再启动 context。
bool acquireFdbusRuntime();
void releaseFdbusRuntime();
}

class Logger;

//worker 端异步日志目标，同时承载同一 FDBus 会话上的等级查询和调节。
struct FdbusTargetConfig
{
    std::string service_name;
    std::size_t queue_capacity;
    std::chrono::milliseconds request_timeout;
    //队列是 worker 的数据容量；请求超时由唯一发送线程逐条应用。
    FdbusTargetConfig()
        : service_name("ipc_log_service"), queue_capacity(2048), request_timeout(2000) {}
};

class FdbusLogTarget : public ILogTarget
{
public:
    //无效配置或 FDBus context 启动失败时抛出异常。
    explicit FdbusLogTarget(const FdbusTargetConfig &config = FdbusTargetConfig());
    ~FdbusLogTarget();
    //成功仅表示进入 worker FIFO；后台发送线程负责等待 FDBus 应答和统计失败。
    bool submit(LogRecord record);
    //等待服务端 flush 屏障；超时可能发生在服务端已开始处理之后。
    FlushResult flush(std::chrono::milliseconds timeout);
    FlushResult shutdown(std::chrono::milliseconds timeout);
    TargetStats stats() const;

    //绑定 worker 的 Logger；目标只持 weak_ptr，避免 Logger 与目标形成强引用环。
    bool attachLogger(const std::shared_ptr<Logger> &logger);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class LogAggregator;

//主进程控制台展示的在线 worker 快照；未知等级通过 level_known 明确表示。
struct WorkerInfo
{
    //session 是当前连接临时 ID；process_instance 才是控制命令的稳定目标键。
    uint64_t session_id;
    uint64_t process_instance;
    uint32_t pid;
    LogLevel level;
    bool level_known;
    std::string status;

    WorkerInfo()
        : session_id(0), process_instance(0), pid(0), level(LogLevel::Info)
        , level_known(false)
    {}
};

struct WorkerControlResult
{
    //confirmed 需要应用级回复；SET 的超时/断连由 uncertain 区分未知结果。
    WorkerInfo worker;
    bool confirmed;
    bool uncertain;
    std::string message;

    WorkerControlResult() : confirmed(false), uncertain(false) {}
};

//服务端先由 onOnline 记录会话，再用定向异步 invoke 查询/设置；命令线程有界等待 onReply。
struct LogServerConfig
{
    std::string service_name;
    std::size_t max_payload_bytes;
    std::size_t max_pending_flushes;
    std::size_t max_worker_instances;
    //限制单包大小、保留的异步 flush 请求数和同时可登记的 worker 会话数。
    LogServerConfig()
        : service_name("ipc_log_service"), max_payload_bytes(8192), max_pending_flushes(128)
        , max_worker_instances(1024) {}
};

class LogServer
{
public:
    LogServer(LogAggregator &aggregator,
              const LogServerConfig &config = LogServerConfig());
    ~LogServer();
    //绑定服务成功返回 true；失败返回 false，构造时的非法配置会抛异常。
    bool start();
    //等待在途控制与 flush 回调并销毁 endpoint；输出排空由 aggregator.shutdown 完成。
    void shutdown(std::chrono::milliseconds timeout);
    //返回当前在线会话快照并异步查询各 worker 的进程实例与等级。
    std::vector<WorkerInfo> listWorkers(std::chrono::milliseconds timeout);
    //按进程实例查询；调用线程等待异步 worker reply，最长不超过 timeout。
    WorkerControlResult getWorkerLevel(uint64_t process_instance,
                                       std::chrono::milliseconds timeout);
    //按进程实例设置独立等级；confirmed 只在 worker 应用应答核对成功后置 true。
    WorkerControlResult setWorkerLevel(uint64_t process_instance, LogLevel level,
                                       std::chrono::milliseconds timeout);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} //namespace ipc_log

#endif
