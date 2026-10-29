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

} //namespace

//服务端主体：维护在线会话和待完成控制命令，接收日志后交给共享汇聚队列。
class LogServer::Impl
{
public:
    struct ProcessState//一个在线 worker 会话的服务端档案
    {
        uint64_t last_seen_sequence;//该会话已接收的最大日志序号
        WorkerInfo info;//已确认的实例号、PID、当前等级和状态
        //会话先于首条日志出现；身份为空时由日志或定向 GET 补齐。
        ProcessState() : last_seen_sequence(0) {}
    };

    struct PendingControl//一条已发出、还没收到回复的控制命令
    {
        //每个 session 仅一个在途控制请求，靠应用 request_id 关联异步应答。
        FdbSessionId_t session;//目标 worker 的会话
        uint64_t request_id;//应用层命令号，用来配对回复
        protocol::LevelControlAction action;//GET 或 SET
        uint32_t requested_level;//SET 时请求的新等级
        std::mutex mutex;//保护 completed 和 result
        std::condition_variable ready;//唤醒等待结果的命令线程
        bool completed;//是否已有结果
        WorkerControlResult result;//回给调用方的控制结果
        PendingControl(FdbSessionId_t sid, uint64_t id,
                       protocol::LevelControlAction kind, uint32_t level)
            : session(sid), request_id(id), action(kind), requested_level(level)
            , completed(false)
        {}
    };

    //FDBus 回调适配层，只转发事件；业务状态全部留在 Impl。
    class Server : public ipc::fdbus::CBaseServer
    {
    public:
        Server(Impl *owner, const std::string &service_name)//保存回调宿主和注册的服务名
            : CBaseServer(service_name.c_str()), owner_(owner)
        {}

        //以下回调只按事件类型转发给 Impl。
        void onInvoke(ipc::fdbus::CBaseJob::Ptr &msg_ref) override//收到日志请求
        {
            owner_->onInvoke(msg_ref);
        }

        void onOnline(const ipc::fdbus::CFdbOnlineInfo &info) override//worker 上线
        {
            owner_->onOnline(info);
        }

        void onOffline(const ipc::fdbus::CFdbOnlineInfo &info) override//worker 下线
        {
            owner_->onOffline(info);
        }

        void onReply(ipc::fdbus::CBaseJob::Ptr &msg_ref) override//控制命令的业务应答
        {
            owner_->onReply(msg_ref);
        }

        void onStatus(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                      int32_t error_code, const char *description) override//控制命令的传输失败
        {
            owner_->onStatus(msg_ref, error_code, description);
        }

    private:
        Impl *owner_;//回调转发的宿主
    };

    //构造只校验配置；FDBus 资源和 service 在 start() 中创建，失败可重新尝试。
    Impl(LogAggregator &aggregator, const LogServerConfig &config)
        : aggregator_(aggregator), config_(config), server_(0), runtime_acquired_(false)
        , running_(false), stopped_(false), next_control_request_id_(1)
    {
        if (config_.service_name.empty() || config_.max_payload_bytes == 0 ||
            config_.max_payload_bytes > 1024 * 1024 ||
            config_.max_worker_instances == 0 ||
            config_.max_worker_instances > 65536)
            throw std::invalid_argument("log server configuration is invalid");
    }

    ~Impl() { shutdown(std::chrono::seconds(5)); }//析构兜底关闭

    //启动服务：创建 FDBus 服务端并绑定 service。
    bool start()
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);//启动和关闭串行执行
        if (running_.load(std::memory_order_acquire)) return true;//已经启动过
        if (stopped_ || !detail::acquireFdbusRuntime()) return false;//已关闭或 runtime 不可用
        runtime_acquired_ = true;
        try
        {
            server_ = new Server(this, config_.service_name);//创建服务端对象
            server_->enableWatchdog(true);//开启 FDBus 看门狗
            const std::string service_url = std::string("svc://") + config_.service_name;
            running_.store(true, std::memory_order_release);//先标运行，再绑定服务
            server_->bind(service_url.c_str());
        }
        catch (...)
        {//创建或绑定失败，回滚已经拿到的资源
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
    //退出日志系统
    void shutdown(std::chrono::milliseconds timeout)
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);//和启动互斥
        if (stopped_) return;//已经关过
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;//所有等待共用这个截止时间
        running_.store(false, std::memory_order_release);//先停止接收新请求
        if (server_)
        {
            server_->unbind();//从 FDBus 注销服务
        }
        {//等已在执行的控制命令结束
            std::unique_lock<std::mutex> lock(process_mutex_);
            control_ready_.wait_until(lock, deadline,
                [this]() { return pending_controls_.empty(); });
        }
        cancelPendingControls();//超时仍未结束的命令直接取消
        if (server_)
        {
            server_->prepareDestroy();//销毁 FDBus 服务端
            delete server_;
            server_ = 0;
        }
        if (runtime_acquired_)
        {//释放启动时拿到的 runtime 引用
            detail::releaseFdbusRuntime();
            runtime_acquired_ = false;
        }
        stopped_ = true;
    }

    //列出在线 worker：逐个查询等级。
    std::vector<WorkerInfo> listWorkers(std::chrono::milliseconds timeout)
    {
        const std::vector<WorkerControlResult> results = queryWorkers(timeout);//先查所有在线 worker
        std::vector<WorkerInfo> workers;
        workers.reserve(results.size());
        for (std::vector<WorkerControlResult>::const_iterator it = results.begin();
             it != results.end(); ++it)
            workers.push_back(it->worker);//只保留对外的 worker 信息
        return workers;
    }

    //查询指定实例的日志等级。
    WorkerControlResult getWorkerLevel(uint64_t process_instance,
                                       std::chrono::milliseconds timeout)
    {
        WorkerControlResult result;
        result.worker.process_instance = process_instance;//失败时也能看出查的是哪个实例
        if (process_instance == 0)//实例号必须非零
        {
            result.message = "process instance must be nonzero";
            result.worker.status = result.message;
            return result;
        }
        if (timeout.count() <= 0)//超时必须为正
        {
            result.message = "control timeout must be positive";
            result.worker.status = result.message;
            return result;
        }
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;//整个调用共用的截止时间
        const FdbSessionId_t session = resolveSession(process_instance, deadline);//先找到对应会话
        if (!fdbValidFdbId(session))//判断是否合法
        {
            result.message = "offline or process instance is unknown";
            result.worker.status = result.message;
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline)//发现阶段已经把时间用完
        {
            result.message = "control timed out during worker discovery";
            result.worker.status = result.message;
            return result;
        }
        return waitControl(startControl(session, protocol::LEVEL_CONTROL_GET, 0,
                                        LogLevel::Info, deadline), deadline);//发 GET 并等结果
    }

    //设置指定实例的日志等级。
    WorkerControlResult setWorkerLevel(uint64_t process_instance, LogLevel level,
                                       std::chrono::milliseconds timeout)
    {
        WorkerControlResult result;
        if (!isValidLogLevel(static_cast<uint32_t>(level)))//等级必须在 DEBUG..ERROR
        {
            result.worker.process_instance = process_instance;
            result.message = "invalid log level";
            result.worker.status = result.message;
            return result;
        }
        if (process_instance == 0)//实例号必须非零
        {
            result.message = "process instance must be nonzero";
            result.worker.status = result.message;
            return result;
        }
        if (timeout.count() <= 0)//超时必须为正
        {
            result.worker.process_instance = process_instance;
            result.message = "control timeout must be positive";
            result.worker.status = result.message;
            return result;
        }

        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + timeout;//整个调用共用的截止时间
        const FdbSessionId_t target_session = resolveSession(process_instance, deadline);//先找到对应会话
        if (!fdbValidFdbId(target_session))
        {
            result.worker.process_instance = process_instance;
            result.message = "worker is offline or its instance could not be resolved";
            result.worker.status = result.message;
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline)//发现阶段已经把时间用完
        {
            result.worker.process_instance = process_instance;
            result.message = "control timed out during worker discovery";
            result.worker.status = result.message;
            return result;
        }
        return waitControl(startControl(target_session, protocol::LEVEL_CONTROL_SET,
                                        process_instance, level, deadline), deadline);//发 SET 并等结果
    }

private:
    //把回复写回 FDBus 请求。
    void reply(ipc::fdbus::CBaseJob::Ptr &msg_ref,
               const protocol::LogReply &response)
    {
        ipc::fdbus::CFdbProtoMsgBuilder builder(response);//把回复序列化成 FDBus 消息
        ipc::fdbus::CFdbMessage::reply(msg_ref, builder);//发回给请求方
    }

    //回复一条失败结果。
    void replyError(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                    uint64_t request_id,
                    protocol::ReplyCode code,
                    const std::string &message)
    {
        protocol::LogReply response;
        response.set_request_id(request_id);//带上请求号，方便发送端配对
        response.set_code(code);//失败原因码
        response.set_accepted(false);
        response.set_message(message);
        reply(msg_ref, response);
    }

    //worker 上线：登记会话。
    void onOnline(const ipc::fdbus::CFdbOnlineInfo &info)
    {
        std::lock_guard<std::mutex> lock(process_mutex_);//会话表由这把锁保护
        if (sessions_.find(info.mSid) == sessions_.end() &&
            sessions_.size() >= config_.max_worker_instances)
            return;//会话数到上限，不再接收新 worker
        ProcessState &state = sessions_[info.mSid];//新会话先建一条空档案
        state.info.session_id = static_cast<uint64_t>(info.mSid);
        state.info.status = "online; identity pending";//身份等首条日志或 GET 补齐
    }

    //worker 下线：删除会话并取消它的在途控制命令。
    void onOffline(const ipc::fdbus::CFdbOnlineInfo &info)
    {
        std::shared_ptr<PendingControl> pending;
        {//先把会话和它的在途命令从表里摘掉
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

    //控制命令的应答回调：校验后唤醒等待者。
    void onReply(ipc::fdbus::CBaseJob::Ptr &msg_ref)
    {
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);//取到底层消息
        if (!message || message->code() != kLevelControlCode) return;//不是控制应答就忽略
        const FdbSessionId_t sid = message->session();//回复来自哪个 worker
        std::shared_ptr<PendingControl> active;
        {//找到该会话正在等的命令
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::const_iterator it =
                pending_controls_.find(sid);
            if (it != pending_controls_.end()) active = it->second;
        }
        if (!active) return;//没有在途命令，说明是迟到回复
        //先校验 wire reply，再确认 session 上仍是同一个请求，过滤迟到或错配应答。
        protocol::LevelControlReply response;
        ipc::fdbus::CFdbProtoMsgParser parser(response);
        if (message->getPayloadSize() < 0 || message->getPayloadSize() > 1024 ||
            !message->deserialize(parser))
        {//包大小不对或 protobuf 解不开
            completeControlFailure(active, "worker returned an invalid control reply",
                                   active->action == protocol::LEVEL_CONTROL_SET);
            return;
        }
        if (response.request_id() != active->request_id ||
            response.process_instance() == 0 || response.pid() == 0)
        {//命令号或身份对不上，不能当成这次请求的结果
            completeControlFailure(active, "worker control reply identity did not match the request",
                                   active->action == protocol::LEVEL_CONTROL_SET);
            return;
        }

        //锁内确认 pending 仍是最初那条，并用回复身份回填会话表；唤醒放到锁外。
        std::shared_ptr<PendingControl> pending;
        bool identity_matches = true;
        {//再次确认表里还是这条请求，并回填 worker 身份
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(sid);
            if (it == pending_controls_.end() ||
                it->second->request_id != response.request_id())
                return;//已经被超时或其他线程处理掉
            pending = it->second;
            std::map<FdbSessionId_t, ProcessState>::iterator session = sessions_.find(sid);
            if (session != sessions_.end() && response.process_instance() != 0 &&
                response.pid() != 0)
            {
                ProcessState &state = session->second;
                identity_matches =
                    (state.info.process_instance == 0 ||
                     state.info.process_instance == response.process_instance()) &&
                    (state.info.pid == 0 || state.info.pid == response.pid());//和首次登记的身份比较
                if (identity_matches)
                {//身份一致才更新会话档案
                    state.info.process_instance = response.process_instance();
                    state.info.pid = response.pid();
                    state.info.level_known = response.level_known() &&
                        isValidLogLevel(response.actual_level());
                    if (state.info.level_known)
                        state.info.level = static_cast<LogLevel>(response.actual_level());
                    state.info.status = response.message();
                }
            }
            pending_controls_.erase(it);//命令已完成，从在途表移除
        }

        //确认需要应用级 OK、身份一致且等级可读；SET 还要求实际等级等于请求等级。
        {//把结果写进命令对象，再唤醒等待的调用线程
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.worker.session_id = static_cast<uint64_t>(sid);
            if (identity_matches)
            {
                pending->result.worker.process_instance = response.process_instance();
                pending->result.worker.pid = response.pid();
            }
            pending->result.worker.level_known = identity_matches && response.level_known() &&
                isValidLogLevel(response.actual_level());//等级字段必须可读
            if (pending->result.worker.level_known)
                pending->result.worker.level = static_cast<LogLevel>(response.actual_level());
            pending->result.worker.status = response.message();
            pending->result.message = response.message();
            pending->result.confirmed = response.result() == protocol::LEVEL_CONTROL_OK &&
                pending->result.worker.level_known && identity_matches;//应用层 OK 且身份一致才算确认
            if (!identity_matches)
                pending->result.message = "worker identity does not match its FDBus session";
            if (pending->action == protocol::LEVEL_CONTROL_SET &&
                pending->result.confirmed &&
                response.actual_level() != pending->requested_level)
            {//SET 要求 worker 回报的等级正好是请求的等级
                pending->result.confirmed = false;
                pending->result.message = "worker reply level does not match requested level";
                pending->result.worker.status = pending->result.message;
            }
            pending->completed = true;
            pending->ready.notify_all();//唤醒 waitControl
        }
        control_ready_.notify_all();
    }

    //控制命令的传输失败回调：结束等待。
    void onStatus(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                  int32_t error_code, const char *description)
    {
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
        if (!message || message->code() != kLevelControlCode) return;//不是控制请求的状态
        const FdbSessionId_t sid = message->session();
        std::shared_ptr<PendingControl> pending;
        {//取出该会话的在途命令并移除
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(sid);
            if (it == pending_controls_.end()) return;//已经超时或被取消
            pending = it->second;
            pending_controls_.erase(it);
        }
        {//把传输错误写成结果，唤醒等待者
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.worker.session_id = static_cast<uint64_t>(sid);
            pending->result.message = std::string("FDBus status ") +
                std::to_string(error_code) + ": " + (description ? description : "no detail");
            pending->result.worker.status = pending->result.message;
            pending->result.uncertain = pending->action == protocol::LEVEL_CONTROL_SET;//SET 无法确认是否已执行
            pending->completed = true;
            pending->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    //发出一条控制命令：登记后 invoke，每个会话同时只允许一条。
    std::shared_ptr<PendingControl> startControl(
        FdbSessionId_t sid, protocol::LevelControlAction action,
        uint64_t target_process_instance, LogLevel level,
        std::chrono::steady_clock::time_point deadline)
    {
        std::shared_ptr<PendingControl> pending;
        std::string error;
        WorkerInfo worker_info;
        {//在锁内找会话并占住这个会话的控制槽位
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, ProcessState>::const_iterator session = sessions_.find(sid);
            if (session == sessions_.end())
                error = "worker session is offline";//会话已经不在了
            else
            {
                worker_info = session->second.info;
                if (pending_controls_.find(sid) != pending_controls_.end())
                    error = "a previous control request is still pending for this session";//上一条还没回
                else
                {//分配命令号并登记，回复靠它配对
                    pending.reset(new PendingControl(sid, next_control_request_id_++, action,
                                                     static_cast<uint32_t>(level)));
                    pending->result.worker = worker_info;
                    pending_controls_[sid] = pending;
                }
            }
        }
        if (!pending)
        {//没能登记，直接造一条已完成的结果返回
            pending.reset(new PendingControl(sid, 0, action, static_cast<uint32_t>(level)));
            pending->result.worker = worker_info;
            pending->result.worker.session_id = static_cast<uint64_t>(sid);
            pending->result.message = error;
            pending->result.worker.status = error;
            pending->completed = true;
            return pending;
        }
        if (!running_.load(std::memory_order_acquire) || !server_)
        {//服务正在退出，不再发命令
            completeControlFailure(pending, "log server is stopping", false);
            return pending;
        }

        //请求在锁外构造，request_id 用登记时分配的值，回复靠它配对。
        protocol::LevelControlRequest request;
        request.set_request_id(pending->request_id);
        request.set_action(action);
        request.set_target_process_instance(target_process_instance);
        if (action == protocol::LEVEL_CONTROL_SET)
            request.set_requested_level(static_cast<uint32_t>(level));//GET 不需要请求等级
        CFdbProtoMsgBuilder builder(request);
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const long long remaining = now < deadline ?
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() : 1;
        const int32_t timeout_ms = static_cast<int32_t>(remaining > 60000 ? 60000 :
                                                        (remaining < 1 ? 1 : remaining));//只等到统一截止时间
        //按 FDBus 在线会话定向异步 invoke，响应由 Server::onReply/onStatus 汇总。
        if (!server_->invoke(sid, kLevelControlCode, builder, timeout_ms))
            completeControlFailure(pending, "FDBus rejected the control request", false);//没发出去，直接失败
        return pending;
    }

    //退出时取消所有在途控制命令。
    void cancelPendingControls()
    {
        std::vector<std::shared_ptr<PendingControl> > pending;
        {//先把所有在途命令搬到局部，再清空表
            std::lock_guard<std::mutex> lock(process_mutex_);
            for (std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                     pending_controls_.begin(); it != pending_controls_.end(); ++it)
                pending.push_back(it->second);
            pending_controls_.clear();
        }
        for (std::vector<std::shared_ptr<PendingControl> >::iterator it = pending.begin();
             it != pending.end(); ++it)
        {//逐条标记取消并唤醒等待线程
            std::lock_guard<std::mutex> lock((*it)->mutex);
            (*it)->result.message = "log server stopped before control reply";
            (*it)->result.worker.status = (*it)->result.message;
            (*it)->result.uncertain = (*it)->action == protocol::LEVEL_CONTROL_SET;
            (*it)->completed = true;
            (*it)->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    //直接结束一条控制命令并报告失败。
    void completeControlFailure(const std::shared_ptr<PendingControl> &pending,
                                const std::string &message, bool uncertain)
    {
        {//只移除仍然指向这条命令的槽位
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, std::shared_ptr<PendingControl> >::iterator it =
                pending_controls_.find(pending->session);
            if (it != pending_controls_.end() && it->second == pending)
                pending_controls_.erase(it);
        }
        {//写入失败结果并唤醒等待者
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result.message = message;
            pending->result.worker.status = message;
            pending->result.uncertain = uncertain;
            pending->completed = true;
            pending->ready.notify_all();
        }
        control_ready_.notify_all();
    }

    //等待控制命令的结果，超时返回未知。
    WorkerControlResult waitControl(const std::shared_ptr<PendingControl> &pending,
                                     std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(pending->mutex);//结果由回调线程写入
        if (!pending->completed && !pending->ready.wait_until(lock, deadline,
            [pending]() { return pending->completed; }))
        {//超时：复制当前结果，标记 SET 为未知
            WorkerControlResult result = pending->result;
            result.message = "timed out waiting for worker control reply";
            result.worker.status = result.message;
            result.uncertain = pending->action == protocol::LEVEL_CONTROL_SET;
            return result;
        }
        return pending->result;//回调已经填好结果
    }

    //按实例号找到会话，找不到就发 GET 扫描一遍。
    FdbSessionId_t resolveSession(uint64_t process_instance,
                                  std::chrono::steady_clock::time_point deadline)
    {
        {//先查已经登记过身份的会话
            std::lock_guard<std::mutex> lock(process_mutex_);
            for (std::map<FdbSessionId_t, ProcessState>::const_iterator it = sessions_.begin();
                 it != sessions_.end(); ++it)
            {
                if (it->second.info.process_instance == process_instance)
                    return it->first;
            }
        }
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline) return FDB_INVALID_ID;//没时间再扫描了
        const long long remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const std::chrono::milliseconds discovery_timeout(remaining > 1 ? remaining / 2 : 1);//扫描只用一半时间
        const std::vector<WorkerControlResult> workers = queryWorkers(discovery_timeout);
        for (std::vector<WorkerControlResult>::const_iterator it = workers.begin();
             it != workers.end(); ++it)
        {//在 GET 结果里找这个实例
            if (it->worker.process_instance == process_instance)
                return static_cast<FdbSessionId_t>(it->worker.session_id);
        }
        return FDB_INVALID_ID;
    }

    //向所有在线 worker 发 GET，收集结果。
    std::vector<WorkerControlResult> queryWorkers(std::chrono::milliseconds timeout)
    {
        std::vector<FdbSessionId_t> sessions;
        {//快照当前在线会话，之后不再持锁
            std::lock_guard<std::mutex> lock(process_mutex_);
            for (std::map<FdbSessionId_t, ProcessState>::const_iterator it = sessions_.begin();
                 it != sessions_.end(); ++it)
                sessions.push_back(it->first);
        }
        std::vector<WorkerControlResult> results;
        results.reserve(sessions.size());
        if (timeout.count() <= 0)
        {//超时不合法，直接给每个会话一个失败结果
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
            std::chrono::steady_clock::now() + timeout;//所有 GET 共用的截止时间
        std::vector<std::shared_ptr<PendingControl> > pending;
        pending.reserve(sessions.size());
        for (std::vector<FdbSessionId_t>::const_iterator it = sessions.begin();
             it != sessions.end(); ++it)
            pending.push_back(startControl(*it, protocol::LEVEL_CONTROL_GET, 0,
                                            LogLevel::Info, deadline));//逐条发出 GET
        for (std::vector<std::shared_ptr<PendingControl> >::const_iterator it = pending.begin();
             it != pending.end(); ++it)
            results.push_back(waitControl(*it, deadline));//再统一收结果
        return results;
    }

    //检查日志记录的字段和长度。
    bool validateRecord(const protocol::LogRecord &record, std::string &error) const
    {
        if (record.timestamp_ms() == 0 || record.pid() == 0 || record.sequence() == 0 ||
            record.process_instance() == 0)
        {//身份字段和时间戳都必须有值
            error = "record identity and timestamp fields must be nonzero";
            return false;
        }
        if (!isValidLogLevel(record.level()))//等级必须在 DEBUG..ERROR
        {
            error = "record level is outside DEBUG..ERROR";
            return false;
        }
        if (record.module().empty() || record.module().size() > kMaxModuleBytes ||
            record.message().size() > kMaxMessageBytes)
        {//模块和正文不能为空，也不能超过字段上限
            error = "record module or message exceeds its limit";
            return false;
        }
        return true;
    }

    //fdbus收到消息的回调函数：判断后提交到汇聚者入队。
    void onInvoke(ipc::fdbus::CBaseJob::Ptr &msg_ref)
    {
        if (!running_.load(std::memory_order_acquire))//服务正在退出
        {
            ipc::fdbus::CFdbMessage::status(msg_ref, FDB_ST_INVALID_ID,
                                             "log server is shutting down");
            return;
        }
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);//取到底层消息
        if (!message || message->code() != kLogRequestCode)//不是日志请求
        {
            ipc::fdbus::CFdbMessage::status(msg_ref, FDB_ST_INVALID_ID, "unknown IPC log request");
            return;
        }
        if (message->getPayloadSize() < 0 ||
            static_cast<std::size_t>(message->getPayloadSize()) > config_.max_payload_bytes)
        {//包太大，不解析
            replyError(msg_ref, 0, protocol::REPLY_BAD_REQUEST, "request payload exceeds limit");
            return;
        }
        protocol::LogRecord incoming;
        ipc::fdbus::CFdbProtoMsgParser parser(incoming);
        if (!message->deserialize(parser))//protobuf 解码失败
        {
            ipc::fdbus::CFdbMessage::status(msg_ref, FDB_ST_MSG_DECODE_FAIL,
                                             "cannot decode IPC log record");
            return;
        }
        handleRecord(msg_ref, incoming);//字段和身份校验在下一步做
    }


    //处理一条日志：校验身份后提交汇聚队列并回复。
    void handleRecord(ipc::fdbus::CBaseJob::Ptr &msg_ref,
                      const protocol::LogRecord &incoming)
    {
        std::string error;
        if (!validateRecord(incoming, error))//字段或长度不合法
        {
            replyError(msg_ref, incoming.sequence(), protocol::REPLY_BAD_REQUEST, error);
            return;
        }
        ipc::fdbus::CFdbMessage *message =
            ipc::fdbus::castToMessage<ipc::fdbus::CFdbMessage *>(msg_ref);
        const FdbSessionId_t sid = message ? message->session() : FDB_INVALID_ID;//消息来自哪个会话
        std::string identity_error;
        {
            //日志身份绑定当前 session，防止不同 client 伪造相同 PID 或实例号。
            std::lock_guard<std::mutex> lock(process_mutex_);
            std::map<FdbSessionId_t, ProcessState>::iterator it = sessions_.find(sid);
            if (it == sessions_.end())
            {
                identity_error = "sender session was not registered as online";//会话不在线
            }
            else if ((it->second.info.process_instance != 0 &&
                      it->second.info.process_instance != incoming.process_instance()) ||
                     (it->second.info.pid != 0 && it->second.info.pid != incoming.pid()))
            {//同一会话中途换了 PID 或实例号
                identity_error = "session identity changed while connected";
            }
            else if (incoming.sequence() <= it->second.last_seen_sequence)
                identity_error = "record sequence is duplicate or stale";//序号没有前进
            else
            {//身份和序号都通过，更新会话档案
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
            replyError(msg_ref, incoming.sequence(), protocol::REPLY_BAD_REQUEST,
                       identity_error);
            return;
        }

        LogRecord record;//协议对象转成内部记录
        record.timestamp_ms = incoming.timestamp_ms();
        record.level = static_cast<LogLevel>(incoming.level());
        record.pid = incoming.pid();
        record.module = incoming.module();
        record.message = incoming.message();
        record.sequence = incoming.sequence();
        record.process_instance = incoming.process_instance();
        if (!aggregator_.submit(record))//共享汇聚队列满或已关闭
        {
            replyError(msg_ref, incoming.sequence(), protocol::REPLY_QUEUE_FULL,
                       "server aggregation queue is full or stopping");
            return;
        }
        protocol::LogReply response;
        response.set_request_id(incoming.sequence());//用日志序号回配
        response.set_code(protocol::REPLY_ACCEPTED);
        response.set_accepted(true);
        //ACCEPTED 只确认进入汇聚队列；最终写出由服务端退出排空完成。
        response.set_message("accepted into the bounded aggregation queue");
        reply(msg_ref, response);
    }

    LogAggregator &aggregator_;//日志最终进入的共享汇聚队列
    LogServerConfig config_;//服务名、包大小和会话上限
    Server *server_;//FDBus 服务端对象
    bool runtime_acquired_;//是否已持有 FDBus runtime 引用
    std::atomic<bool> running_;//是否正在接收请求
    bool stopped_;//shutdown 是否已完成
    uint64_t next_control_request_id_;//下一条控制命令的应用层编号
    std::mutex lifecycle_mutex_;//保护 start/shutdown 的全过程
    std::mutex process_mutex_;//保护会话表和待完成命令表
    std::condition_variable control_ready_;//shutdown 等待在途命令结束
    std::map<FdbSessionId_t, ProcessState> sessions_;//在线会话表，按 FDBus 会话号索引
    std::map<FdbSessionId_t, std::shared_ptr<PendingControl> > pending_controls_;//每个会话最多一条在途命令
};

//公开接口只做转发；服务状态、锁和回调全部由 Impl 管理。
LogServer::LogServer(LogAggregator &aggregator, const LogServerConfig &config)
    : impl_(new Impl(aggregator, config))
{}
LogServer::~LogServer() { shutdown(std::chrono::seconds(5)); }//析构兜底关闭
bool LogServer::start() { return impl_->start(); }//绑定 service
void LogServer::shutdown(std::chrono::milliseconds timeout) { if (impl_) impl_->shutdown(timeout); }//关闭 service
std::vector<WorkerInfo> LogServer::listWorkers(std::chrono::milliseconds timeout)
{ return impl_->listWorkers(timeout); }//列出在线 worker
WorkerControlResult LogServer::getWorkerLevel(uint64_t process_instance,
                                               std::chrono::milliseconds timeout)
{ return impl_->getWorkerLevel(process_instance, timeout); }//查询指定实例的等级
WorkerControlResult LogServer::setWorkerLevel(uint64_t process_instance, LogLevel level,
                                               std::chrono::milliseconds timeout)
{ return impl_->setWorkerLevel(process_instance, level, timeout); }//设置指定实例的等级

} //namespace ipc_log
