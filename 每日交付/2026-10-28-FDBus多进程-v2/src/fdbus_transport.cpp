#include "ipc_log/fdbus_transport.h"

#include "ipc_log/async_queue.h"
#include "ipc_log/logger.h"
#include "log_protocol.pb.h"

#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CFdbProtoMsgBuilder.h>
#include <fdbus/CFdbBaseObject.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ipc_log {

namespace detail {
namespace {
std::mutex &runtimeMutex() { static std::mutex value; return value; }
std::size_t &runtimeReferences() { static std::size_t value = 0; return value; }
bool &runtimeDestroyed() { static bool value = false; return value; }
}

bool acquireFdbusRuntime()
{
    std::lock_guard<std::mutex> lock(runtimeMutex());
    if (runtimeDestroyed()) return false;
    if (runtimeReferences() == 0)
    {
        ipc::fdbus::CFdbContext::enableLogger(false);
        if (!ipc::fdbus::FDB_CONTEXT->start()) return false;
    }
    ++runtimeReferences();
    return true;
}

void releaseFdbusRuntime()
{
    std::lock_guard<std::mutex> lock(runtimeMutex());
    if (runtimeReferences() == 0) return;
    if (--runtimeReferences() == 0)
    {
        runtimeDestroyed() = true;
        ipc::fdbus::FDB_CONTEXT->destroy();
    }
}
}

namespace {

using namespace ipc::fdbus;

struct RequestState
{
    //发送线程等此状态；FDBus 回调写 reply，submitted 用于区分确定失败与结果未知。
    std::mutex mutex;
    std::condition_variable ready;
    bool done;
    bool transport_error;
    bool uncertain;
    bool submitted;
    std::string error;
    uint64_t expected_request_id;
    protocol::LogReply reply;
    RequestState()
        : done(false), transport_error(false), uncertain(false), submitted(false)
        , expected_request_id(0)
    {}
};

//调用方已持有 state->mutex；统一记录传输错误和“请求可能已到服务端”的边界。
void failRequest(RequestState &state, const std::string &error)
{
    state.transport_error = true;
    state.uncertain = state.submitted;
    state.error = error;
}

//FDBus reply 回调只校验并保存应答；等待线程负责把协议结果映射成计数。
void completeRequest(const std::shared_ptr<RequestState> &state,
                     CBaseJob::Ptr &message_ref, CFdbBaseObject *)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->done) return;
    CFdbMessage *message = castToMessage<CFdbMessage *>(message_ref);
    if (!message)
    {
        failRequest(*state, "FDBus returned a non-message response");
    }
    else if (message->code() != kLogRequestCode)
    {
        failRequest(*state, "FDBus reply message code does not match the request");
    }
    else if (message->isStatus())
    {
        int32_t code = 0;
        std::string reason;
        message->decodeStatus(code, reason);
        failRequest(*state, reason.empty() ? "FDBus status " + std::to_string(code) : reason);
    }
    else
    {
        protocol::LogReply reply;
        CFdbProtoMsgParser parser(reply);
        if (!message->deserialize(parser))
        {
            failRequest(*state, "unable to decode IPC log reply");
        }
        else
        {
            if (reply.request_id() != state->expected_request_id)
                failRequest(*state, "FDBus reply request_id does not match the pending request");
            else
                state->reply = reply;
        }
    }
    state->done = true;
    state->ready.notify_all();
}

} //namespace

class FdbusLogTarget::Impl
{
public:
    class Client : public CBaseClient
    {
    public:
        Client(Impl *owner, const std::string &endpoint_name)
            : CBaseClient(endpoint_name.c_str()), owner_(owner)
        {}

        void onOnline(const CFdbOnlineInfo &) override
        {
            owner_->setOnline(true);
        }

        void onOffline(const CFdbOnlineInfo &) override
        {
            owner_->setOnline(false);
        }

        void onInvoke(CBaseJob::Ptr &msg_ref) override
        {
            //反向控制走独立回调，不占用 worker 的日志 FIFO 或发送线程。
            owner_->onControlInvoke(msg_ref);
        }

        bool send(const protocol::LogRecord &record,
                  const std::shared_ptr<RequestState> &state,
                  int timeout_ms)
        {
            CFdbProtoMsgBuilder builder(record);
            return invoke(kLogRequestCode, builder,
                          [state](CBaseJob::Ptr &message_ref, CFdbBaseObject *object)
                          { completeRequest(state, message_ref, object); },
                          0, timeout_ms);
        }

    private:
        Impl *owner_;
    };

    explicit Impl(const FdbusTargetConfig &config)
        : config_(config), queue_(config.queue_capacity)
        , client_(0), accepting_(true), stopping_(false), online_(false)
        , next_sequence_(1), accepted_(0), dropped_(0), rejected_(0), uncertain_(0)
        , pid_(currentProcessId()), process_instance_(createProcessInstanceId(pid_))
        , runtime_acquired_(false), stopped_(false), sender_done_(false)
    {
        //先验证边界，再启动 context、client 和唯一发送线程；构造失败时逐项回滚。
        if (config_.service_name.empty() || config_.queue_capacity == 0 ||
            config_.queue_capacity > 65536 ||
            config_.request_timeout.count() <= 0 ||
            config_.request_timeout.count() > 60000 ||
            config_.shutdown_timeout.count() <= 0 ||
            config_.shutdown_timeout.count() > 60000)
            throw std::invalid_argument("FDBus target configuration is invalid");
        runtime_acquired_ = detail::acquireFdbusRuntime();
        if (!runtime_acquired_) throw std::runtime_error("unable to start FDBus context");
        try
        {
            const std::string endpoint_name = "ipc_log_worker_" + std::to_string(pid_);
            client_ = new Client(this, endpoint_name);
            client_->enableReconnect(true);
            const std::string service_url = std::string("svc://") + config_.service_name;
            client_->connect(service_url.c_str());
            sender_ = std::thread(&Impl::run, this);
        }
        catch (...)
        {
            if (client_)
            {
                client_->prepareDestroy();
                delete client_;
                client_ = 0;
            }
            detail::releaseFdbusRuntime();
            runtime_acquired_ = false;
            throw;
        }
    }

    ~Impl() { shutdown(); }

    bool attachLogger(const std::shared_ptr<Logger> &logger)
    {
        //控制回调需要更新 Logger；弱引用避免 Logger 与 transport 互相延长寿命。
        if (!logger || !accepting_.load(std::memory_order_acquire) ||
            stopping_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(logger_mutex_);
        if (!logger_.expired()) return false;
        logger_ = logger;
        return true;
    }

    bool submit(LogRecord record)
    {
        if (!accepting_.load(std::memory_order_acquire))
        {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::lock_guard<std::mutex> lock(submit_mutex_);
        if (!accepting_.load(std::memory_order_relaxed))
        {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        //worker 的日志身份由 FDBus target 持有，和服务端会话映射及控制命令使用同一实例号。
        record.pid = pid_;
        record.process_instance = process_instance_;
        record.sequence = next_sequence_;
        if (!queue_.tryPush(std::move(record)))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++next_sequence_;
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    ShutdownResult shutdown()
    {
        //拒绝新记录、等待发送线程排空本地队列，再销毁 client 和共享 context。
        std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
        ShutdownResult result;
        if (stopped_)
        {
            result.already_stopped = true;
            result.message = "log target is already stopped";
            result.stats = stats();
            return result;
        }
        if (std::this_thread::get_id() == sender_.get_id())
        {
            result.drained = false;
            result.message = "shutdown cannot run on the sender thread";
            result.stats = stats();
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(submit_mutex_);
            accepting_.store(false, std::memory_order_release);
        }
        queue_.close();
        //关闭只阻止新项；发送线程会把关闭前已入队的记录逐条发出。
        {
            std::unique_lock<std::mutex> lock(sender_mutex_);
            sender_done_cv_.wait_for(lock, config_.shutdown_timeout,
                                     [this]() { return sender_done_; });
            result.drained = sender_done_;
        }
        if (!result.drained)
        {
            //超时后强制停止：发送线程丢弃剩余记录并退出，避免离线服务拖住进程。
            stopping_.store(true, std::memory_order_release);
            online_ready_.notify_all();
        }
        if (sender_.joinable()) sender_.join();
        if (client_)
        {
            client_->disconnect();
            client_->prepareDestroy();
            delete client_;
            client_ = 0;
        }
        if (runtime_acquired_)
        {
            detail::releaseFdbusRuntime();
            runtime_acquired_ = false;
        }
        stopped_ = true;
        result.stats = stats();
        if (!result.drained)
            result.message = "local send queue did not drain before the shutdown timeout";
        else if (result.stats.dropped || result.stats.rejected || result.stats.uncertain)
            result.message = "shutdown completed with dropped, rejected, or uncertain records";
        return result;
    }

    uint64_t processInstance() const { return process_instance_; }

    TargetStats stats() const
    {
        TargetStats result;
        result.accepted = accepted_.load(std::memory_order_relaxed);
        result.dropped = dropped_.load(std::memory_order_relaxed);
        result.rejected = rejected_.load(std::memory_order_relaxed);
        result.uncertain = uncertain_.load(std::memory_order_relaxed);
        return result;
    }

private:
    void onControlInvoke(CBaseJob::Ptr &msg_ref)
    {
        //目标身份来自此 FDBus client 实例；只有成功回复 SET 才代表等级已更新。
        CFdbMessage *message = castToMessage<CFdbMessage *>(msg_ref);
        if (!message || message->code() != kLevelControlCode)
        {
            CFdbMessage::status(msg_ref, FDB_ST_INVALID_ID, "unknown worker control request");
            return;
        }
        //验证控制消息、目标实例和等级后更新附加 Logger，最后返回实际等级快照。
        protocol::LevelControlRequest request;
        CFdbProtoMsgParser parser(request);
        if (message->getPayloadSize() < 0 || message->getPayloadSize() > 1024 ||
            !message->deserialize(parser) || request.request_id() == 0)
        {
            CFdbMessage::status(msg_ref, FDB_ST_BAD_PARAMETER, "invalid worker control request");
            return;
        }

        protocol::LevelControlReply response;
        response.set_request_id(request.request_id());
        response.set_process_instance(process_instance_);
        response.set_pid(pid_);
        if (!accepting_.load(std::memory_order_acquire) ||
            stopping_.load(std::memory_order_acquire))
        {
            response.set_result(protocol::LEVEL_CONTROL_STOPPING);
            response.set_message("worker is stopping");
        }
        else if (request.action() != protocol::LEVEL_CONTROL_GET &&
                 request.action() != protocol::LEVEL_CONTROL_SET)
        {
            response.set_result(protocol::LEVEL_CONTROL_BAD_REQUEST);
            response.set_message("unsupported level control action");
        }
        else if ((request.action() == protocol::LEVEL_CONTROL_SET &&
                  request.target_process_instance() != process_instance_) ||
                 (request.action() == protocol::LEVEL_CONTROL_GET &&
                  request.target_process_instance() != 0 &&
                  request.target_process_instance() != process_instance_))
        {
            response.set_result(protocol::LEVEL_CONTROL_TARGET_MISMATCH);
            response.set_message("request targets a different process instance");
        }
        else if (request.action() == protocol::LEVEL_CONTROL_SET &&
                 !isValidLogLevel(request.requested_level()))
        {
            response.set_result(protocol::LEVEL_CONTROL_INVALID_LEVEL);
            response.set_message("requested level is outside DEBUG..ERROR");
        }
        else
        {
            const std::shared_ptr<Logger> logger = loggerSnapshot();
            if (!logger)
            {
                response.set_result(protocol::LEVEL_CONTROL_NOT_READY);
                response.set_message("worker logger is not attached");
            }
            else
            {
                if (request.action() == protocol::LEVEL_CONTROL_SET)
                    logger->setLevel(static_cast<LogLevel>(request.requested_level()));
                response.set_result(protocol::LEVEL_CONTROL_OK);
                response.set_level_known(true);
                response.set_actual_level(static_cast<uint32_t>(logger->level()));
                response.set_message(request.action() == protocol::LEVEL_CONTROL_SET ?
                                     "worker log level updated" : "worker log level queried");
            }
        }

        CFdbProtoMsgBuilder builder(response);
        CFdbMessage::reply(msg_ref, builder);
    }

    std::shared_ptr<Logger> loggerSnapshot()
    {
        std::lock_guard<std::mutex> lock(logger_mutex_);
        return logger_.lock();
    }

    void setOnline(bool online)
    {
        //在线标志由独立互斥锁保护；状态变化唤醒唯一发送线程。
        std::lock_guard<std::mutex> lock(online_mutex_);
        online_ = online;
        online_ready_.notify_all();
    }

    bool waitUntilOnline()
    {
        //发送队列允许先于服务上线积累；关闭也会唤醒等待，避免线程卡在连接门闩。
        std::unique_lock<std::mutex> lock(online_mutex_);
        online_ready_.wait(lock, [this]()
        { return online_ || stopping_.load(std::memory_order_acquire); });
        return online_ && !stopping_.load(std::memory_order_acquire);
    }

    bool sendRequest(const protocol::LogRecord &record,
                     const std::shared_ptr<RequestState> &state)
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->expected_request_id = record.sequence();
        }
        //只有发送线程等待 FDBus 应答，Logger 调用在入队后立即返回。
        if (!waitUntilOnline()) return false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->submitted = true;
        }
        const bool queued = client_->send(record, state,
            static_cast<int>(config_.request_timeout.count()));
        if (!queued)
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->submitted = false;
            return false;
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->ready.wait_for(lock, config_.request_timeout + std::chrono::milliseconds(100),
                                   [state]() { return state->done; }))
        {
            state->transport_error = true;
            state->uncertain = true;
            state->error = "FDBus response deadline elapsed";
            state->done = true;
            state->ready.notify_all();
            return true;
        }
        return true;
    }

    void run()
    {
        LogRecord record;
        //单消费者按 FIFO 逐条 invoke 并等待应答，保证每个 worker 最多一个在途请求。
        while (queue_.waitPop(record))
        {
            if (stopping_.load(std::memory_order_acquire))
            {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            protocol::LogRecord message;
            message.set_timestamp_ms(record.timestamp_ms);
            message.set_level(static_cast<uint32_t>(record.level));
            message.set_pid(record.pid);
            message.set_module(record.module);
            message.set_message(record.message);
            message.set_sequence(record.sequence);
            message.set_process_instance(record.process_instance);

            const std::shared_ptr<RequestState> state(new RequestState());
            //记录应答区分未发送丢弃、传输结果未知和服务端明确拒绝。
            const bool sent = sendRequest(message, state);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!sent)
                dropped_.fetch_add(1, std::memory_order_relaxed);
            else if (state->transport_error)
                uncertain_.fetch_add(1, std::memory_order_relaxed);
            else if (!state->reply.accepted() ||
                     state->reply.code() != protocol::REPLY_ACCEPTED)
                rejected_.fetch_add(1, std::memory_order_relaxed);
        }
        //通知 shutdown：本地队列已经取空，发送线程即将退出。
        {
            std::lock_guard<std::mutex> lock(sender_mutex_);
            sender_done_ = true;
        }
        sender_done_cv_.notify_all();
    }

    FdbusTargetConfig config_;
    BoundedQueue<LogRecord> queue_;
    Client *client_;
    std::thread sender_;
    std::mutex logger_mutex_;
    std::weak_ptr<Logger> logger_;
    mutable std::mutex submit_mutex_;
    std::mutex shutdown_mutex_;
    std::mutex online_mutex_;
    std::condition_variable online_ready_;
    std::mutex sender_mutex_;
    std::condition_variable sender_done_cv_;
    bool online_;
    std::atomic<bool> accepting_;
    std::atomic<bool> stopping_;
    uint64_t next_sequence_;
    std::atomic<uint64_t> accepted_;
    std::atomic<uint64_t> dropped_;
    std::atomic<uint64_t> rejected_;
    std::atomic<uint64_t> uncertain_;
    const uint32_t pid_;
    const uint64_t process_instance_;
    bool runtime_acquired_;
    bool stopped_;
    bool sender_done_;
};

FdbusLogTarget::FdbusLogTarget(const FdbusTargetConfig &config)
    : impl_(new Impl(config))
{}

FdbusLogTarget::~FdbusLogTarget() { shutdown(); }
bool FdbusLogTarget::submit(LogRecord record) { return impl_->submit(record); }
ShutdownResult FdbusLogTarget::shutdown() { return impl_->shutdown(); }
TargetStats FdbusLogTarget::stats() const { return impl_->stats(); }
uint64_t FdbusLogTarget::processInstance() const { return impl_->processInstance(); }
bool FdbusLogTarget::attachLogger(const std::shared_ptr<Logger> &logger)
{ return impl_->attachLogger(logger); }

} //namespace ipc_log
