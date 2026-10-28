#include "ipc_log/fdbus_transport.h"
#include "ipc_log/log_aggregator.h"

#include "log_protocol.pb.h"

#include <fdbus/CBaseServer.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CFdbProtoMsgBuilder.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ipc_log {
namespace {

using namespace ipc::fdbus;

struct PendingReplies
{
    //异步 flush 保留 FDBus 请求引用；shutdown 等此计数归零后才销毁 context。
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t count;
    bool cancelled;
    explicit PendingReplies() : count(0), cancelled(false) {}
};

} //namespace

class LogServer::Impl
{
public:
    //会话键负责定位 client；WorkerInfo 保存已校验身份，序号单独拒绝重放。
    struct ProcessState
    {
        uint64_t last_seen_sequence;
        WorkerInfo info;
        //会话先于首条日志出现；身份为空时由日志或定向 GET 补齐。
        ProcessState() : last_seen_sequence(0) {}
    };

    struct PendingControl
    {
        //每个 session 仅一个在途控制请求，靠应用 request_id 关联异步应答。
        FdbSessionId_t session;
        uint64_t request_id;
        protocol::LevelControlAction action;
        uint32_t requested_level;
        std::mutex mutex;
        std::condition_variable ready;
        bool completed;
        WorkerControlResult result;
        PendingControl(FdbSessionId_t sid, uint64_t id,
                       protocol::LevelControlAction kind, uint32_t level)
            : session(sid), request_id(id), action(kind), requested_level(level)
            , completed(false)
        {}
    };

    class Server : public ipc::fdbus::CBaseServer
    {
    public:
        Server(Impl *owner, const std::string &service_name)
            : CBaseServer(service_name.c_str()), owner_(owner)
        {}

        void onInvoke(ipc::fdbus::CBaseJob::Ptr &msg_ref) override
        {
            owner_->onInvoke(msg_ref);
        }

        void onOnline(const ipc::fdbus::CFdbOnlineInfo &info) override
        {
            owner_->onOnline(info);
        }

        void onOffline(const ipc::fdbus::CFdbOnlineInfo &info) override
        {
            owner_->onOffline(info);
        }

        void onReply(ipc::fdbus::CBaseJob::Ptr &msg_ref) override
        {
            owner_->onReply(msg_ref);
        }

        void onStatus(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                      int32_t error_code, const char *description) override
        {
            owner_->onStatus(msg_ref, error_code, description);
        }

    private:
        Impl *owner_;
    };

    Impl(LogAggregator &aggregator, const LogServerConfig &config)
        : aggregator_(aggregator), config_(config), server_(0), runtime_acquired_(false)
        , running_(false), stopped_(false), pending_(new PendingReplies())
        , next_control_request_id_(1)
    {
        if (config_.service_name.empty() || config_.max_payload_bytes == 0 ||
            config_.max_payload_bytes > 1024 * 1024 || config_.max_pending_flushes == 0 ||
            config_.max_pending_flushes > 4096 || config_.max_worker_instances == 0 ||
            config_.max_worker_instances > 65536)
            throw std::invalid_argument("log server configuration is invalid");
    }

    ~Impl() { shutdown(std::chrono::seconds(5)); }

    //启动 FDBus runtime 和 service；失败时回滚本对象已创建的 endpoint/runtime。
    bool start()
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (running_.load(std::memory_order_acquire)) return true;
        if (stopped_ || !detail::acquireFdbusRuntime()) return false;
        runtime_acquired_ = true;
        try
        {
            server_ = new Server(this, config_.service_name);
            server_->enableWatchdog(true);
            const std::string service_url = std::string("svc://") + config_.service_name;
            running_.store(true, std::memory_order_release);
            server_->bind(service_url.c_str());
        }
        catch (...)
        {
            if (server_)
            {
                server_->prepareDestroy();
                delete server_;
                server_ = 0;
            }
            running_.store(false, std::memory_order_release);
            detail::releaseFdbusRuntime();
            runtime_acquired_ = false;
            return false;
        }
        return true;
    }

    void shutdown(std::chrono::milliseconds timeout)
    {
        //停止接收、等控制与 flush 回调后再销毁 endpoint/context；输出排空交给 aggregator.shutdown。
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (stopped_) return;
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;
        running_.store(false, std::memory_order_release);
        if (server_)
        {
            server_->unbind();
        }
        {
            std::unique_lock<std::mutex> lock(process_mutex_);
            control_ready_.wait_until(lock, deadline,
                [this]() { return pending_controls_.empty(); });
        }
        cancelPendingControls();
        {
            std::unique_lock<std::mutex> lock(pending_->mutex);
            pending_->ready.wait_until(lock, deadline,
                [this]() { return pending_->count == 0; });
            if (pending_->count != 0) pending_->cancelled = true;
        }
        if (server_)
        {
            server_->prepareDestroy();
            delete server_;
            server_ = 0;
        }
        if (runtime_acquired_)
        {
            bool pending_drained = false;
            {
                std::lock_guard<std::mutex> lock(pending_->mutex);
                pending_drained = pending_->count == 0;
            }
            if (pending_drained)
                detail::releaseFdbusRuntime();
            //保留 FDBus context，避免超时屏障的延迟回复引用被提前销毁。
            runtime_acquired_ = false;
        }
        stopped_ = true;
    }

    std::vector<WorkerInfo> listWorkers(std::chrono::milliseconds timeout)
    {
        //GET 可发现已上线但尚未发送首条日志的 worker，并回填其等级快照。
        const std::vector<WorkerControlResult> results = queryWorkers(timeout);
        std::vector<WorkerInfo> workers;
        workers.reserve(results.size());
        for (std::vector<WorkerControlResult>::const_iterator it = results.begin();
             it != results.end(); ++it)
            workers.push_back(it->worker);
        return workers;
    }

    WorkerControlResult getWorkerLevel(uint64_t process_instance,
                                       std::chrono::milliseconds timeout)
    {
        //先按实例号定位会话，再只向该 worker 发一次 GET；未知实例显式报告。
        WorkerControlResult result;
        result.worker.process_instance = process_instance;
        if (process_instance == 0)
        {
            result.message = "process instance must be nonzero";
            result.worker.status = result.message;
            return result;
        }
        if (timeout.count() <= 0)
        {
            result.message = "control timeout must be positive";
            result.worker.status = result.message;
            return result;
        }
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;
        const FdbSessionId_t session = resolveSession(process_instance, deadline);
        if (!fdbValidFdbId(session))
        {
            result.message = "offline or process instance is unknown";
            result.worker.status = result.message;
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            result.message = "control timed out during worker discovery";
            result.worker.status = result.message;
            return result;
        }
        return waitControl(startControl(session, protocol::LEVEL_CONTROL_GET, 0,
                                        LogLevel::Info, deadline), deadline);
    }

    WorkerControlResult setWorkerLevel(uint64_t process_instance, LogLevel level,
                                       std::chrono::milliseconds timeout)
    {
        //目标通过实例号定位，不依赖 PID；未识别会话时先 GET 发现再定向 SET。
        WorkerControlResult result;
        if (!isValidLogLevel(static_cast<uint32_t>(level)))
        {
            result.worker.process_instance = process_instance;
            result.message = "invalid log level";
            result.worker.status = result.message;
            return result;
        }
        if (process_instance == 0)
        {
            result.message = "process instance must be nonzero";
            result.worker.status = result.message;
            return result;
        }
        if (timeout.count() <= 0)
        {
            result.worker.process_instance = process_instance;
            result.message = "control timeout must be positive";
            result.worker.status = result.message;
            return result;
        }

        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;
        const FdbSessionId_t target_session = resolveSession(process_instance, deadline);
        if (!fdbValidFdbId(target_session))
        {
            result.worker.process_instance = process_instance;
            result.message = "worker is offline or its instance could not be resolved";
            result.worker.status = result.message;
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            result.worker.process_instance = process_instance;
            result.message = "control timed out during worker discovery";
            result.worker.status = result.message;
            return result;
        }
        return waitControl(startControl(target_session, protocol::LEVEL_CONTROL_SET,
                                        process_instance, level, deadline), deadline);
    }

private:
    void reply(ipc::fdbus::CBaseJob::Ptr &msg_ref,
               const protocol::LogReply &response)
    {
        ipc::fdbus::CFdbProtoMsgBuilder builder(response);
        ipc::fdbus::CFdbMessage::reply(msg_ref, builder);
    }

    void replyError(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                    uint64_t request_id,
                    protocol::ReplyCode code,
                    const std::string &message)
    {
        protocol::LogReply response;
        response.set_request_id(request_id);
        response.set_code(code);
        response.set_accepted(false);
        response.set_message(message);
        reply(msg_ref, response);
    }

    void onOnline(const ipc::fdbus::CFdbOnlineInfo &info)
    {
        //在线会话表使尚未产生日志的 worker 也能被 list 命令发现。
        std::lock_guard<std::mutex> lock(process_mutex_);
        if (sessions_.find(info.mSid) == sessions_.end() &&
            sessions_.size() >= config_.max_worker_instances)
            return;
        ProcessState &state = sessions_[info.mSid];
        state.info.session_id = static_cast<uint64_t>(info.mSid);
        state.info.status = "online; identity pending";
    }

    void onOffline(const ipc::fdbus::CFdbOnlineInfo &info)
    {
        std::shared_ptr<PendingControl> pending;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            sessions_.erase(info.mSid);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(info.mSid);
            if (it != pending_controls_.end())
            {
                pending = it->second;
                pending_controls_.erase(it);
            }
        }
        //解除会话索引后再唤醒等待者，避免在 process_mutex_ 下拿 PendingControl 锁。
        if (pending)
        {
            //断连取消该会话的在途控制；SET 没有应用应答时只能报告结果未知。
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.worker.session_id = static_cast<uint64_t>(info.mSid);
            pending->result.worker.status = "worker went offline during control request";
            pending->result.message = pending->result.worker.status;
            pending->result.uncertain = pending->action == protocol::LEVEL_CONTROL_SET;
            pending->completed = true;
            pending->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    void onReply(ipc::fdbus::CBaseJob::Ptr &msg_ref)
    {
        //响应按 FDBus session 和应用 request_id 双重匹配，超时后的迟到回复不会误配新命令。
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
        if (!message || message->code() != kLevelControlCode) return;
        const FdbSessionId_t sid = message->session();
        std::shared_ptr<PendingControl> active;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::const_iterator it =
                pending_controls_.find(sid);
            if (it != pending_controls_.end()) active = it->second;
        }
        if (!active) return;
        //先校验 wire reply，再确认 session 上仍是同一个请求，过滤迟到或错配应答。
        protocol::LevelControlReply response;
        ipc::fdbus::CFdbProtoMsgParser parser(response);
        if (message->getPayloadSize() < 0 || message->getPayloadSize() > 1024 ||
            !message->deserialize(parser))
        {
            completeControlFailure(active, "worker returned an invalid control reply",
                                   active->action == protocol::LEVEL_CONTROL_SET);
            return;
        }
        if (response.request_id() != active->request_id ||
            response.process_instance() == 0 || response.pid() == 0)
        {
            completeControlFailure(active, "worker control reply identity did not match the request",
                                   active->action == protocol::LEVEL_CONTROL_SET);
            return;
        }

        std::shared_ptr<PendingControl> pending;
        bool identity_matches = true;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(sid);
            if (it == pending_controls_.end() ||
                it->second->request_id != response.request_id())
                return;
            pending = it->second;
            std::map<FdbSessionId_t, ProcessState>::iterator session = sessions_.find(sid);
            if (session != sessions_.end() && response.process_instance() != 0 &&
                response.pid() != 0)
            {
                ProcessState &state = session->second;
                identity_matches =
                    (state.info.process_instance == 0 ||
                     state.info.process_instance == response.process_instance()) &&
                    (state.info.pid == 0 || state.info.pid == response.pid());
                if (identity_matches)
                {
                    state.info.process_instance = response.process_instance();
                    state.info.pid = response.pid();
                    state.info.level_known = response.level_known() &&
                        isValidLogLevel(response.actual_level());
                    if (state.info.level_known)
                        state.info.level = static_cast<LogLevel>(response.actual_level());
                    state.info.status = response.message();
                }
            }
            pending_controls_.erase(it);
        }

        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.worker.session_id = static_cast<uint64_t>(sid);
            if (identity_matches)
            {
                pending->result.worker.process_instance = response.process_instance();
                pending->result.worker.pid = response.pid();
            }
            pending->result.worker.level_known = identity_matches && response.level_known() &&
                isValidLogLevel(response.actual_level());
            if (pending->result.worker.level_known)
                pending->result.worker.level = static_cast<LogLevel>(response.actual_level());
            pending->result.worker.status = response.message();
            pending->result.message = response.message();
            pending->result.confirmed = response.result() == protocol::LEVEL_CONTROL_OK &&
                pending->result.worker.level_known && identity_matches;
            if (!identity_matches)
                pending->result.message = "worker identity does not match its FDBus session";
            if (pending->action == protocol::LEVEL_CONTROL_SET &&
                pending->result.confirmed &&
                response.actual_level() != pending->requested_level)
            {
                pending->result.confirmed = false;
                pending->result.message = "worker reply level does not match requested level";
                pending->result.worker.status = pending->result.message;
            }
            pending->completed = true;
            pending->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    void onStatus(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                  int32_t error_code, const char *description)
    {
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
        if (!message || message->code() != kLevelControlCode) return;
        const FdbSessionId_t sid = message->session();
        std::shared_ptr<PendingControl> pending;
        //FDBus 状态回调代表传输失败而非业务成功；SET 未获应用应答时结果标记为未知。
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(sid);
            if (it == pending_controls_.end()) return;
            pending = it->second;
            pending_controls_.erase(it);
        }
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.worker.session_id = static_cast<uint64_t>(sid);
            pending->result.message = std::string("FDBus status ") +
                std::to_string(error_code) + ": " + (description ? description : "no detail");
            pending->result.worker.status = pending->result.message;
            pending->result.uncertain = pending->action == protocol::LEVEL_CONTROL_SET;
            pending->completed = true;
            pending->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    std::shared_ptr<PendingControl> startControl(
        FdbSessionId_t sid, protocol::LevelControlAction action,
        uint64_t target_process_instance, LogLevel level,
        std::chrono::steady_clock::time_point deadline)
    {
        //每个 session 同时只保留一个控制请求，避免无法携带应用 request_id 的状态回复串线。
        //先在锁内预留 session 槽位，解锁后才 invoke，避免 FDBus 回调重入时死锁。
        std::shared_ptr<PendingControl> pending;
        std::string error;
        WorkerInfo worker_info;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, ProcessState>::const_iterator session = sessions_.find(sid);
            if (session == sessions_.end())
                error = "worker session is offline";
            else
            {
                worker_info = session->second.info;
                if (pending_controls_.find(sid) != pending_controls_.end())
                    error = "a previous control request is still pending for this session";
                else
                {
                    pending.reset(new PendingControl(sid, next_control_request_id_++, action,
                                                     static_cast<uint32_t>(level)));
                    pending->result.worker = worker_info;
                    pending_controls_[sid] = pending;
                }
            }
        }
        if (!pending)
        {
            pending.reset(new PendingControl(sid, 0, action, static_cast<uint32_t>(level)));
            pending->result.worker = worker_info;
            pending->result.worker.session_id = static_cast<uint64_t>(sid);
            pending->result.message = error;
            pending->result.worker.status = error;
            pending->completed = true;
            return pending;
        }
        if (!running_.load(std::memory_order_acquire) || !server_)
        {
            completeControlFailure(pending, "log server is stopping", false);
            return pending;
        }

        protocol::LevelControlRequest request;
        request.set_request_id(pending->request_id);
        request.set_action(action);
        request.set_target_process_instance(target_process_instance);
        if (action == protocol::LEVEL_CONTROL_SET)
            request.set_requested_level(static_cast<uint32_t>(level));
        CFdbProtoMsgBuilder builder(request);
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const long long remaining = now < deadline ?
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() : 1;
        const int32_t timeout_ms = static_cast<int32_t>(remaining > 60000 ? 60000 :
                                                        (remaining < 1 ? 1 : remaining));
        //按 FDBus 在线会话定向异步 invoke，响应由 Server::onReply/onStatus 汇总。
        if (!server_->invoke(sid, kLevelControlCode, builder, timeout_ms))
            completeControlFailure(pending, "FDBus rejected the control request", false);
        return pending;
    }

    void cancelPendingControls()
    {
        //复制并清空索引后再逐个唤醒等待者；SET 未确认时报告结果未知。
        std::vector<std::shared_ptr<PendingControl> > pending;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            for (std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                     pending_controls_.begin(); it != pending_controls_.end(); ++it)
                pending.push_back(it->second);
            pending_controls_.clear();
        }
        for (std::vector<std::shared_ptr<PendingControl> >::iterator it = pending.begin();
             it != pending.end(); ++it)
        {
            std::lock_guard<std::mutex> lock((*it)->mutex);
            (*it)->result.message = "log server stopped before control reply";
            (*it)->result.worker.status = (*it)->result.message;
            (*it)->result.uncertain = (*it)->action == protocol::LEVEL_CONTROL_SET;
            (*it)->completed = true;
            (*it)->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    void completeControlFailure(const std::shared_ptr<PendingControl> &pending,
                                const std::string &message, bool uncertain)
    {
        //仅移除仍指向此对象的槽位，避免旧超时清掉后续请求。
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(pending->session);
            if (it != pending_controls_.end() && it->second == pending)
                pending_controls_.erase(it);
        }
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.message = message;
            pending->result.worker.status = message;
            pending->result.uncertain = uncertain;
            pending->completed = true;
            pending->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    WorkerControlResult waitControl(const std::shared_ptr<PendingControl> &pending,
                                     std::chrono::steady_clock::time_point deadline)
    {
        //超时只结束本调用等待，不伪造 worker 已执行或未执行 SET 的结论。
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (!pending->completed && !pending->ready.wait_until(lock, deadline,
            [pending]() { return pending->completed; }))
        {
            WorkerControlResult result = pending->result;
            result.message = "timed out waiting for worker control reply";
            result.worker.status = result.message;
            result.uncertain = pending->action == protocol::LEVEL_CONTROL_SET;
            return result;
        }
        return pending->result;
    }

    FdbSessionId_t resolveSession(uint64_t process_instance,
                                  std::chrono::steady_clock::time_point deadline)
    {
        //先按实例号直接命中会话；身份尚未登记时，用一次有界 GET 扫描补齐。
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            for (std::map<FdbSessionId_t, ProcessState>::const_iterator it = sessions_.begin();
                 it != sessions_.end(); ++it)
            {
                if (it->second.info.process_instance == process_instance)
                    return it->first;
            }
        }
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline) return FDB_INVALID_ID;
        const long long remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const std::chrono::milliseconds discovery_timeout(remaining > 1 ? remaining / 2 : 1);
        const std::vector<WorkerControlResult> workers = queryWorkers(discovery_timeout);
        for (std::vector<WorkerControlResult>::const_iterator it = workers.begin();
             it != workers.end(); ++it)
        {
            if (it->worker.process_instance == process_instance)
                return static_cast<FdbSessionId_t>(it->worker.session_id);
        }
        return FDB_INVALID_ID;
    }

    std::vector<WorkerControlResult> queryWorkers(std::chrono::milliseconds timeout)
    {
        //先快照会话列表，再并行发送 GET，所有等待共用同一截止时间。
        std::vector<FdbSessionId_t> sessions;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            for (std::map<FdbSessionId_t, ProcessState>::const_iterator it = sessions_.begin();
                 it != sessions_.end(); ++it)
                sessions.push_back(it->first);
        }
        std::vector<WorkerControlResult> results;
        results.reserve(sessions.size());
        if (timeout.count() <= 0)
        {
            for (std::vector<FdbSessionId_t>::const_iterator it = sessions.begin();
                 it != sessions.end(); ++it)
            {
                WorkerControlResult result;
                result.worker.session_id = static_cast<uint64_t>(*it);
                result.message = "control timeout must be positive";
                result.worker.status = result.message;
                results.push_back(result);
            }
            return results;
        }

        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;
        std::vector<std::shared_ptr<PendingControl> > pending;
        pending.reserve(sessions.size());
        for (std::vector<FdbSessionId_t>::const_iterator it = sessions.begin();
             it != sessions.end(); ++it)
            pending.push_back(startControl(*it, protocol::LEVEL_CONTROL_GET, 0,
                                            LogLevel::Info, deadline));
        for (std::vector<std::shared_ptr<PendingControl> >::const_iterator it = pending.begin();
             it != pending.end(); ++it)
            results.push_back(waitControl(*it, deadline));
        return results;
    }

    bool validateRecord(const protocol::LogRecord &record, std::string &error) const
    {
        //这里检查 wire 边界；session 身份和单调序号在 handleRecord 中单独校验。
        if (record.timestamp_ms() == 0 || record.pid() == 0 || record.sequence() == 0 ||
            record.process_instance() == 0)
        {
            error = "record identity and timestamp fields must be nonzero";
            return false;
        }
        if (!isValidLogLevel(record.level()))
        {
            error = "record level is outside DEBUG..ERROR";
            return false;
        }
        if (record.module().empty() || record.module().size() > kMaxModuleBytes ||
            record.message().size() > kMaxMessageBytes)
        {
            error = "record module or message exceeds its limit";
            return false;
        }
        return true;
    }

    void onInvoke(ipc::fdbus::CBaseJob::Ptr &msg_ref)
    {
        //FDBus 回调只做长度/协议路由和快速入队；flush 的最终应答由屏障回调发送。
        if (!running_.load(std::memory_order_acquire))
        {
            ipc::fdbus::CFdbMessage::status(msg_ref, FDB_ST_INVALID_ID,
                                             "log server is shutting down");
            return;
        }
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
        if (!message || message->code() != kLogRequestCode)
        {
            ipc::fdbus::CFdbMessage::status(msg_ref, FDB_ST_INVALID_ID, "unknown IPC log request");
            return;
        }
        if (message->getPayloadSize() < 0 ||
            static_cast<std::size_t>(message->getPayloadSize()) > config_.max_payload_bytes)
        {
            replyError(msg_ref, 0, protocol::REPLY_BAD_REQUEST, "request payload exceeds limit");
            return;
        }
        protocol::LogRequest request;
        ipc::fdbus::CFdbProtoMsgParser parser(request);
        if (!message->deserialize(parser))
        {
            ipc::fdbus::CFdbMessage::status(msg_ref, FDB_ST_MSG_DECODE_FAIL,
                                             "cannot decode IPC log request");
            return;
        }
        if (request.kind() == protocol::REQUEST_LOG_RECORD && request.has_record())
        {
            handleRecord(msg_ref, request);
            return;
        }
        if (request.kind() == protocol::REQUEST_FLUSH && request.has_flush())
        {
            handleFlush(msg_ref, request);
            return;
        }
        replyError(msg_ref, request.request_id(), protocol::REPLY_BAD_REQUEST,
                   "request kind and payload do not match");
    }


    void handleRecord(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                      const protocol::LogRequest &request)
    {
        //校验请求与会话身份后入共享汇聚队列；ACK 只表示入队成功。
        const protocol::LogRecord &incoming = request.record();
        std::string error;
        if (!validateRecord(incoming, error))
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_BAD_REQUEST, error);
            return;
        }
        if (request.request_id() != incoming.sequence())
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_BAD_REQUEST,
                       "log request_id must match record sequence");
            return;
        }
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
        const FdbSessionId_t sid = message ? message->session() : FDB_INVALID_ID;
        std::string identity_error;
        {
            //日志身份绑定当前 session，防止不同 client 伪造相同 PID 或实例号。
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, ProcessState>::iterator it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                identity_error = "sender session was not registered as online";
            }
            else if ((it->second.info.process_instance != 0 &&
                      it->second.info.process_instance != incoming.process_instance()) ||
                     (it->second.info.pid != 0 && it->second.info.pid != incoming.pid()))
            {
                identity_error = "session identity changed while connected";
            }
            else if (incoming.sequence() <= it->second.last_seen_sequence)
                identity_error = "record sequence is duplicate or stale";
            else
            {
                ProcessState &state = it->second;
                state.last_seen_sequence = incoming.sequence();
                state.info.session_id = static_cast<uint64_t>(sid);
                state.info.process_instance = incoming.process_instance();
                state.info.pid = incoming.pid();
                state.info.status = "online";
            }
        }
        if (!identity_error.empty())
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_BAD_REQUEST,
                       identity_error);
            return;
        }

        LogRecord record;
        record.timestamp_ms = incoming.timestamp_ms();
        record.level = static_cast<LogLevel>(incoming.level());
        record.pid = incoming.pid();
        record.module = incoming.module();
        record.message = incoming.message();
        record.sequence = incoming.sequence();
        record.process_instance = incoming.process_instance();
        if (!aggregator_.submit(record))
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_QUEUE_FULL,
                       "server aggregation queue is full or stopping");
            return;
        }
        protocol::LogReply response;
        response.set_request_id(request.request_id());
        response.set_code(protocol::REPLY_ACCEPTED);
        response.set_accepted(true);
        //ACCEPTED 只确认进入汇聚队列，输出刷新由后续 flush 屏障确认。
        response.set_message("accepted into the bounded aggregation queue");
        reply(msg_ref, response);
    }

    bool reservePending()
    {
        //先预留异步应答额度，限制持有的 FDBus 请求引用数量。
        std::lock_guard<std::mutex> lock(pending_->mutex);
        if (pending_->count >= config_.max_pending_flushes) return false;
        ++pending_->count;
        return true;
    }

    void releasePending()
    {
        //无论屏障成功、失败还是队列拒绝，都释放此前预留的异步应答名额。
        std::lock_guard<std::mutex> lock(pending_->mutex);
        if (pending_->count) --pending_->count;
        pending_->ready.notify_all();
    }

    void handleFlush(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                     const protocol::LogRequest &request)
    {
        //核对 worker 屏障目标序号，再保留请求对象直到此前记录已写出并刷新 Sink。
        const protocol::FlushRequest &flush = request.flush();
        if (flush.request_id() == 0 || flush.request_id() != request.request_id() ||
            flush.process_instance() == 0)
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_BAD_REQUEST,
                       "flush request identity is invalid");
            return;
        }
        std::string identity_error;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            ipc::fdbus::CFdbMessage *message =
                ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
            const FdbSessionId_t sid = message ? message->session() : FDB_INVALID_ID;
            std::map<FdbSessionId_t, ProcessState>::iterator it = sessions_.find(sid);
            if (it == sessions_.end())
                identity_error = "flush sender session is not online";
            else if (it->second.info.process_instance != 0 &&
                     it->second.info.process_instance != flush.process_instance())
                identity_error = "flush process instance does not match its session";
            else if (flush.target_sequence() > it->second.last_seen_sequence)
                identity_error = "flush sequence has not been accepted by the server";
            else if (it->second.info.process_instance == 0)
            {
                //无日志 worker 也可用已验证的 FDBus session 建立实例身份并正常确认空屏障。
                it->second.info.process_instance = flush.process_instance();
            }
        }
        if (!identity_error.empty())
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_BAD_REQUEST, identity_error);
            return;
        }
        if (!reservePending())
        {
            replyError(msg_ref, request.request_id(), protocol::REPLY_QUEUE_FULL,
                       "too many pending flush requests");
            return;
        }

        const std::shared_ptr<PendingReplies> pending = pending_;
        const uint64_t request_id = request.request_id();
        const std::shared_ptr<ipc::fdbus::CBaseJob::Ptr> retained(
            new ipc::fdbus::CBaseJob::Ptr(msg_ref));
        //请求引用随屏障保留，直到此前日志写入并刷新两个 Sink。
        const bool queued = aggregator_.flushAsync(
            [pending, retained, request_id](const FlushResult &result)
            {
                protocol::LogReply response;
                response.set_request_id(request_id);
                const bool barrier_completed = result.status == FlushStatus::Success ||
                                               result.status == FlushStatus::Partial;
                response.set_accepted(barrier_completed);
                response.set_code(barrier_completed ? protocol::REPLY_FLUSHED :
                                                      protocol::REPLY_FLUSH_FAILED);
                response.set_message(result.message);
                response.set_dropped(result.dropped);
                response.set_rejected(result.rejected);
                response.set_uncertain(result.uncertain);
                response.set_write_errors(result.write_errors);
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (!pending->cancelled)
                {
                    ipc::fdbus::CFdbProtoMsgBuilder builder(response);
                    ipc::fdbus::CFdbMessage::reply(*retained, builder);
                }
                if (pending->count) --pending->count;
                pending->ready.notify_all();
            });
        if (!queued)
        {
            releasePending();
            replyError(msg_ref, request.request_id(), protocol::REPLY_QUEUE_FULL,
                       "server flush control reserve is full");
        }
    }

    LogAggregator &aggregator_;
    LogServerConfig config_;
    Server *server_;
    bool runtime_acquired_;
    std::atomic<bool> running_;
    bool stopped_;
    std::shared_ptr<PendingReplies> pending_;
    uint64_t next_control_request_id_;
    std::mutex lifecycle_mutex_;
    std::mutex process_mutex_;
    std::condition_variable control_ready_;
    std::map<FdbSessionId_t, ProcessState> sessions_;
    std::map<FdbSessionId_t, std::shared_ptr<PendingControl> > pending_controls_;
};

LogServer::LogServer(LogAggregator &aggregator, const LogServerConfig &config)
    : impl_(new Impl(aggregator, config))
{}
LogServer::~LogServer() { shutdown(std::chrono::seconds(5)); }
bool LogServer::start() { return impl_->start(); }
void LogServer::shutdown(std::chrono::milliseconds timeout) { if (impl_) impl_->shutdown(timeout); }
std::vector<WorkerInfo> LogServer::listWorkers(std::chrono::milliseconds timeout)
{ return impl_->listWorkers(timeout); }
WorkerControlResult LogServer::getWorkerLevel(uint64_t process_instance,
                                               std::chrono::milliseconds timeout)
{ return impl_->getWorkerLevel(process_instance, timeout); }
WorkerControlResult LogServer::setWorkerLevel(uint64_t process_instance, LogLevel level,
                                               std::chrono::milliseconds timeout)
{ return impl_->setWorkerLevel(process_instance, level, timeout); }

} //namespace ipc_log
