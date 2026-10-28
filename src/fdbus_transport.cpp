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

//FDBus reply 回调只校验并保存应答；等待线程负责把协议结果映射成 FlushResult。
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
    struct Item
    {
        //记录和 flush 共用 FIFO；Flush 项携带此前已接收入队的序号屏障。
        enum Kind { Record, Flush } kind;
        LogRecord record;
        uint64_t request_id;
        uint64_t target_sequence;
        std::shared_ptr<RequestState> state;
        Item() : kind(Record), request_id(0), target_sequence(0) {}
    };

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

        bool send(const protocol::LogRequest &request,
                  const std::shared_ptr<RequestState> &state,
                  int timeout_ms)
        {
            CFdbProtoMsgBuilder builder(request);
            return invoke(kLogRequestCode, builder,
                          [state](CBaseJob::Ptr &message_ref, CFdbBaseObject *object)
                          { completeRequest(state, message_ref, object); },
                          0, timeout_ms);
        }

    private:
        Impl *owner_;
    };

    explicit Impl(const FdbusTargetConfig &config)
        : config_(config), queue_(config.queue_capacity, 1)
        , client_(0), accepting_(true), stopping_(false), online_(false)
        , next_sequence_(1), next_request_id_(1), accepted_(0), dropped_(0)
        , rejected_(0), uncertain_(0)
        , pid_(currentProcessId()), process_instance_(createProcessInstanceId(pid_))
        , runtime_acquired_(false), stopped_(false)
    {
        //先验证边界，再启动 context、client 和唯一发送线程；构造失败时逐项回滚。
        if (config_.service_name.empty() || config_.queue_capacity == 0 ||
            config_.queue_capacity > 65536 ||
            config_.request_timeout.count() <= 0 ||
            config_.request_timeout.count() > 60000)
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

    ~Impl() { shutdown(std::chrono::seconds(5)); }

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
        Item item;
        item.kind = Item::Record;
        item.record = std::move(record);
        if (!queue_.tryPush(std::move(item)))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++next_sequence_;
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    FlushResult flush(std::chrono::milliseconds timeout)
    {
        //同步 flush 只等待队列中的屏障应答；普通 Logger 调用不经过此等待路径。
        if (timeout.count() <= 0)
        {
            FlushResult result;
            result.status = FlushStatus::Timeout;
            result.message = "flush timeout must be positive";
            fillStats(result);
            return result;
        }
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::timed_mutex> serial(flush_mutex_, std::defer_lock);
        if (!serial.try_lock_until(deadline))
        {
            FlushResult result;
            result.status = FlushStatus::Timeout;
            result.message = "timed out waiting for another remote flush";
            fillStats(result);
            return result;
        }
        const std::shared_ptr<RequestState> state(new RequestState());
        {
            std::lock_guard<std::mutex> lock(submit_mutex_);
            Item item;
            item.kind = Item::Flush;
            item.request_id = next_request_id_++;
            item.target_sequence = next_sequence_ - 1;
            item.state = state;
            if (!queue_.tryPushControl(std::move(item)))
            {
                FlushResult result;
                result.status = FlushStatus::QueueFull;
                result.message = "flush control reserve is full or target is stopped";
                fillStats(result);
                return result;
            }
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->ready.wait_until(lock, deadline, [state]() { return state->done; }))
        {
            FlushResult result;
            result.status = FlushStatus::Timeout;
            result.message = "remote flush timed out; its result may be unknown";
            result.uncertain = uncertain_.load(std::memory_order_relaxed) +
                               (state->submitted ? 1 : 0);
            result.dropped = dropped_.load(std::memory_order_relaxed);
            result.rejected = rejected_.load(std::memory_order_relaxed);
            return result;
        }
        FlushResult result = toFlushResult(*state);
        result.dropped += dropped_.load(std::memory_order_relaxed);
        result.rejected += rejected_.load(std::memory_order_relaxed);
        result.uncertain += uncertain_.load(std::memory_order_relaxed);
        if (result.status == FlushStatus::Success &&
            (result.dropped || result.rejected || result.uncertain || result.write_errors))
        {
            result.status = FlushStatus::Partial;
            result.message = "flush completed with dropped, rejected, uncertain, or failed records";
        }
        return result;
    }

    FlushResult shutdown(std::chrono::milliseconds timeout)
    {
        //拒绝新记录、尽力等待屏障、唤醒发送线程，再销毁 client 和共享 context。
        std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
        if (stopped_)
        {
            FlushResult result;
            result.status = FlushStatus::Stopped;
            result.message = "log target is already stopped";
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(submit_mutex_);
            accepting_.store(false, std::memory_order_release);
        }
        const FlushResult result = flush(timeout);
        stopping_.store(true, std::memory_order_release);
        online_ready_.notify_all();
        queue_.close();
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
        return result;
    }

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

    bool sendRequest(const protocol::LogRequest &request,
                     const std::shared_ptr<RequestState> &state)
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->expected_request_id = request.request_id();
        }
        //只有发送线程等待 FDBus 应答，Logger 调用在入队后立即返回。
        if (!waitUntilOnline()) return false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->submitted = true;
        }
        const bool queued = client_->send(request, state,
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

    static FlushResult toFlushResult(const RequestState &state)
    {
        //传输失败与服务端应用级拒绝分开映射，避免把未确认写盘说成成功。
        FlushResult result;
        if (state.transport_error)
        {
            result.status = FlushStatus::TransportError;
            result.message = state.error;
            result.uncertain = state.uncertain ? 1 : 0;
            return result;
        }
        result.dropped = state.reply.dropped();
        result.rejected = state.reply.rejected();
        result.uncertain = state.reply.uncertain();
        result.write_errors = state.reply.write_errors();
        if (state.reply.code() == protocol::REPLY_FLUSHED && state.reply.accepted())
            result.status = FlushStatus::Success;
        else
        {
            result.status = (state.reply.code() == protocol::REPLY_FLUSH_FAILED &&
                             state.reply.write_errors() != 0) ?
                            FlushStatus::WriteError : FlushStatus::TransportError;
            result.message = state.reply.message();
        }
        return result;
    }

    void fillStats(FlushResult &result) const
    {
        //队列侧累计计数附加到快速失败的 flush 结果中。
        result.dropped = dropped_.load(std::memory_order_relaxed);
        result.rejected = rejected_.load(std::memory_order_relaxed);
        result.uncertain = uncertain_.load(std::memory_order_relaxed);
    }

    void run()
    {
        Item item;
        //单消费者按 FIFO 逐条 invoke 并等待应答，保证每个 worker 最多一个在途请求。
        while (queue_.waitPop(item))
        {
            if (stopping_.load(std::memory_order_acquire))
            {
                if (item.kind == Item::Record) dropped_.fetch_add(1, std::memory_order_relaxed);
                else if (item.state)
                {
                    std::lock_guard<std::mutex> lock(item.state->mutex);
                    item.state->transport_error = true;
                    item.state->error = "target stopped before flush was sent";
                    item.state->done = true;
                    item.state->ready.notify_all();
                }
                continue;
            }

            //按 Item 构造协议消息；发送端单线程保持每个 worker 最多一个在途请求。
            protocol::LogRequest request;
            request.set_request_id(item.kind == Item::Record ? item.record.sequence : item.request_id);
            if (item.kind == Item::Record)
            {
                request.set_kind(protocol::REQUEST_LOG_RECORD);
                protocol::LogRecord *record = request.mutable_record();
                record->set_timestamp_ms(item.record.timestamp_ms);
                record->set_level(static_cast<uint32_t>(item.record.level));
                record->set_pid(item.record.pid);
                record->set_module(item.record.module);
                record->set_message(item.record.message);
                record->set_sequence(item.record.sequence);
                record->set_process_instance(item.record.process_instance);
            }
            else
            {
                request.set_kind(protocol::REQUEST_FLUSH);
                protocol::FlushRequest *flush = request.mutable_flush();
                flush->set_request_id(item.request_id);
                flush->set_process_instance(process_instance_);
                flush->set_target_sequence(item.target_sequence);
            }

            const std::shared_ptr<RequestState> state = item.state ?
                item.state : std::shared_ptr<RequestState>(new RequestState());
            //记录应答区分未发送丢弃、传输结果未知和服务端明确拒绝。
            const bool sent = sendRequest(request, state);
            if (item.kind == Item::Record)
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!sent)
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                else if (state->transport_error)
                    uncertain_.fetch_add(1, std::memory_order_relaxed);
                else if (!state->reply.accepted() ||
                         state->reply.code() != protocol::REPLY_ACCEPTED)
                    rejected_.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                if (item.state)
                {
                    std::lock_guard<std::mutex> lock(item.state->mutex);
                    if (!sent)
                    {
                        item.state->transport_error = true;
                        item.state->uncertain = item.state->submitted;
                        item.state->error = "FDBus service is offline during shutdown";
                        item.state->done = true;
                        item.state->ready.notify_all();
                    }
                }
            }
        }
    }

    FdbusTargetConfig config_;
    BoundedQueue<Item> queue_;
    Client *client_;
    std::thread sender_;
    std::mutex logger_mutex_;
    std::weak_ptr<Logger> logger_;
    mutable std::mutex submit_mutex_;
    std::timed_mutex flush_mutex_;
    std::mutex shutdown_mutex_;
    std::mutex online_mutex_;
    std::condition_variable online_ready_;
    bool online_;
    std::atomic<bool> accepting_;
    std::atomic<bool> stopping_;
    uint64_t next_sequence_;
    uint64_t next_request_id_;
    std::atomic<uint64_t> accepted_;
    std::atomic<uint64_t> dropped_;
    std::atomic<uint64_t> rejected_;
    std::atomic<uint64_t> uncertain_;
    const uint32_t pid_;
    const uint64_t process_instance_;
    bool runtime_acquired_;
    bool stopped_;
};

FdbusLogTarget::FdbusLogTarget(const FdbusTargetConfig &config)
    : impl_(new Impl(config))
{}

FdbusLogTarget::~FdbusLogTarget() { shutdown(std::chrono::seconds(5)); }
bool FdbusLogTarget::submit(LogRecord record) { return impl_->submit(record); }
FlushResult FdbusLogTarget::flush(std::chrono::milliseconds timeout)
{ return impl_->flush(timeout); }
FlushResult FdbusLogTarget::shutdown(std::chrono::milliseconds timeout)
{ return impl_->shutdown(timeout); }
TargetStats FdbusLogTarget::stats() const { return impl_->stats(); }
bool FdbusLogTarget::attachLogger(const std::shared_ptr<Logger> &logger)
{ return impl_->attachLogger(logger); }

} //namespace ipc_log
