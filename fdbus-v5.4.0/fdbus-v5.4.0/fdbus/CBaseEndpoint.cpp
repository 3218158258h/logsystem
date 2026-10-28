/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fdbus/CBaseEndpoint.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbSession.h>
#include <fdbus/CFdbMessage.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CApiSecurityConfig.h>
#include <fdbus/CBaseSocketFactory.h>
#include "CIntraNameProxy.h"
#include "CFdbWatchdog.h"
#include <utils/Log.h>
#ifdef CONFIG_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace ipc {
namespace fdbus {
class CQueryPeerSessionMsg : public CFdbMessage
{
public:
    CQueryPeerSessionMsg(FdbMsgCode_t code, CBaseJob::Ptr &req, EFdbQOS qos)
        : CFdbMessage(code, qos)
        , mReq(req)
    {}
    CBaseJob::Ptr mReq;
protected:
    void onAsyncError(Ptr &ref, FdbMsgStatusCode code, const char *reason)
    {
        auto reply_msg = castToMessage<CFdbMessage *>(mReq);
        reply_msg->status(mReq, code, reason);
    }
};

CBaseEndpoint::CBaseEndpoint(const char *name, CBaseWorker *worker, CFdbBaseContext *context,
                             EFdbEndpointRole role)
    : CFdbBaseObject(name, worker, context, role)
    , mContext(0)
    , mInsecureSessionCnt(0)
    , mSecureSessionCnt(0)
    , mSnAllocator(1)
    , mEpid(FDB_INVALID_ID)
    , mEventRouter(this)
    , mKeepAliveInterval(0)
    , mKeepAliveRetries(0)
    , mSSLContext(0)
    , mExportableLevel(FDB_EXPORTABLE_DOMAIN)
    , mInstanceId(FDB_DEFAULT_INSTANCE)
{
    autoRemove(false);
    enableBlockingMode(false);
    mFlag |= FDB_EP_ALLOW_TCP_NORMAL | FDB_EP_ALLOW_TCP_SECURE | FDB_EP_ENABLE_UDP;
    mObjId = FDB_OBJECT_MAIN;
    mEndpoint = this;
    setContext(context);
}

CBaseEndpoint::~CBaseEndpoint()
{
    if (!mSessionContainer.getContainer().empty())
    {
        LOG_E("~CBaseEndpoint: Unable to destroy context since there are active sessions!\n");
    }
    destroySelf(false);
}

void CBaseEndpoint::prepareDestroy()
{
    autoRemove(false);
    CFdbBaseObject::prepareDestroy();
    destroySelf(true);
}

void CBaseEndpoint::addSocket(CFdbSessionContainer *container)
{
    insertEntry(container->skid(), container);
}

void CBaseEndpoint::deleteSocket(FdbSocketId_t skid)
{
    if (fdbValidFdbId(skid))
    {
        CFdbSessionContainer *container = 0;
        (void)retrieveEntry(skid, container);
        if (container)
        {
            delete container; // also deleted from socket container
        }
    }
    else
    {
        EntryContainer_t &containers = getContainer();
        while (!containers.empty())
        {
            auto it = containers.begin();
            delete it->second; // also deleted from socket container
        }
    }
}

void CBaseEndpoint::getDefaultSvcUrl(std::string &svc_url)
{
    svc_url = FDB_URL_SVC;
    if (mNsName.empty())
    {
        svc_url += mName;
    }
    else
    {
        svc_url += mNsName;
    }
}

EFdbSecureType CBaseEndpoint::QosToSecureType(EFdbQOS qos)
{
    EFdbSecureType sec_type;
    switch (qos)
    {
        case FDB_QOS_SECURE_RELIABLE:
        case FDB_QOS_SECURE_BEST_EFFORTS:
        case FDB_QOS_LOCAL:
            sec_type = FDB_SEC_SECURE;
        break;
        case FDB_QOS_TRY_SECURE_RELIABLE:
        case FDB_QOS_TRY_SECURE_BEST_EFFORTS:
            sec_type = FDB_SEC_TRY_SECURE;
        break;
        default:
            sec_type = FDB_SEC_INSECURE;
        break;
    }

    return sec_type;
}

CFdbSession *CBaseEndpoint::preferredPeer(EFdbQOS qos)
{
    CFdbSession *fallback_session = 0;
    auto &containers = getContainer();
    auto sec_type = QosToSecureType(qos);
    for (auto it = containers.begin(); it != containers.end(); ++it)
    {
        auto sessions = it->second;
        auto session = sessions->getDefaultSession();
        if (!session)
        {
            continue;
        }
        CFdbSocketInfo sinfo;
        sessions->getSocketInfo(sinfo);
        if (sinfo.mAddress->mType == FDB_SOCKET_IPC)
        {
            return session;
        }

        auto session_secure = session->isSecure();
        switch (sec_type)
        {
            case FDB_SEC_INSECURE:
                if (!session_secure)
                {
                    return session;
                }
            break;
            case FDB_SEC_SECURE:
                if (session_secure)
                {
                    return session;
                }
            break;
            default:
                fallback_session = session;
                if (session_secure)
                {
                    return fallback_session;
                }
            break;
        }
    }
    return fallback_session;
}

void CBaseEndpoint::checkAutoRemove()
{
    if (autoRemove() && !getSessionCount())
    {
        prepareDestroy();
        delete this;
    }
}

void CBaseEndpoint::getUrlList(std::vector<std::string> &url_list)
{
    auto &containers = getContainer();
    for (auto it = containers.begin(); it != containers.end(); ++it)
    {
        auto container = it->second;
        CFdbSocketInfo info;
        container->getSocketInfo(info);
        url_list.push_back(info.mAddress->mUrl);
    }
}

FdbObjectId_t CBaseEndpoint::addObject(CFdbBaseObject *obj)
{
    auto obj_id = obj->objId();
    if (obj_id == FDB_OBJECT_MAIN)
    {
        return FDB_INVALID_ID;
    }

    if (mObjectContainer.findEntry(obj))
    {
        return obj_id;
    }

    if (!fdbValidFdbId(obj_id))
    {
        do
        {
            obj_id = mObjectContainer.allocateEntityId();
        } while (obj_id == FDB_OBJECT_MAIN);
    }

    if (obj->role() == FDB_OBJECT_ROLE_SERVER)
    {
        auto o = findObject(obj_id, true);
        if (o)
        {
            if (o == obj)
            {
                return obj_id;
            }
            else
            {
                LOG_E("CBaseEndpoint: server object %d already exist!\n", obj_id);
                return FDB_INVALID_ID;
            }
        }
    }

    obj_id = FDB_OBJECT_MAKE_ID(mSnAllocator++, obj_id);
    obj->objId(obj_id);
    if (mObjectContainer.insertEntry(obj_id, obj))
    {
        return FDB_INVALID_ID;
    }

    obj->enableMigrate(true);
    bool is_first_secure = true;
    bool is_first_insecure = true;
    auto &containers = getContainer();
    for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
    {
        auto container = socket_it->second;
        if (!container->mConnectedSessionTable.empty())
        {
            // get a snapshot of the table to avoid modification of the table in callback
            auto tbl = container->mConnectedSessionTable;
            for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
            {
                auto session = *session_it;
                if (!obj->authentication(session))
                {
                    continue;
                }

                bool is_first;
                if (mRole == FDB_OBJECT_ROLE_CLIENT)
                {
                    if (session->isSecure())
                    {
                        is_first = is_first_secure;
                    }
                    else
                    {
                        is_first = is_first_insecure;
                    }
                }
                else
                {
                    is_first = is_first_secure && is_first_insecure;
                }
                if (session->isSecure())
                {
                    is_first_secure = false;
                }
                else
                {
                    is_first_insecure = false;
                }
                obj->notifyOnline(session, is_first);
            }
        }
    }

    return obj_id;
}

void CBaseEndpoint::removeObject(CFdbBaseObject *obj)
{
    if (!mObjectContainer.findEntry(obj))
    {
        return;
    }

    auto secure_session_cnt = mSecureSessionCnt;
    auto insecure_session_cnt = mInsecureSessionCnt;
    auto &containers = getContainer();
    for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
    {
        auto container = socket_it->second;
        if (!container->mConnectedSessionTable.empty())
        {
            // get a snapshot of the table to avoid modification of the table in callback
            auto tbl = container->mConnectedSessionTable;
            for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
            {
                auto session = *session_it;
                bool is_last;
                if (onlineSeparateChannel())
                {
                    if (session->isSecure())
                    {
                        is_last = secure_session_cnt == 1;
                    }
                    else
                    {
                        is_last = insecure_session_cnt == 1;
                    }
                }
                else
                {
                    is_last = (secure_session_cnt + insecure_session_cnt) == 1;
                }

                if (session->isSecure())
                {
                    if (secure_session_cnt > 0)
                    {
                        secure_session_cnt--;
                    }
                }
                else
                {
                    if (insecure_session_cnt > 0)
                    {
                        insecure_session_cnt--;
                    }
                }

                obj->notifyOffline(*session_it, is_last);
            }
        }
    }

    obj->enableMigrate(false);
    if (obj->mWorker)
    {
        obj->mWorker->flush();
    }
    mObjectContainer.deleteEntry(obj->objId());
    if (obj->autoRemove())
    {
        delete obj;
    }
    // obj->objId(FDB_INVALID_ID);
}

void CBaseEndpoint::unsubscribeSession(CFdbSession *session)
{
    auto &object_tbl = mObjectContainer.getContainer();
    for (auto it = object_tbl.begin(); it != object_tbl.end(); ++it)
    {
        auto object = it->second;
        object->unsubscribe(session);
    }

    unsubscribe(session);
}

class CKickOutSessionJob : public CMethodJob<CBaseEndpoint>
{
public:
    CKickOutSessionJob(CBaseEndpoint *object, FdbSessionId_t sid)
        : CMethodJob<CBaseEndpoint>(object, &CBaseEndpoint::callKickOutSession, JOB_FORCE_RUN)
        , mSid(sid)
    {
    }

    FdbSessionId_t mSid;
};

void CBaseEndpoint::callKickOutSession(CMethodJob<CBaseEndpoint> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CKickOutSessionJob *>(job);
    deleteSession(the_job->mSid);
}

void CBaseEndpoint::kickOut(FdbSessionId_t sid)
{
    mContext->sendAsyncEndeavor(new CKickOutSessionJob(this, sid));
}

CFdbBaseObject *CBaseEndpoint::getObject(CFdbMessage *msg, bool server_only)
{
    auto obj_id = msg->objectId();
    if (obj_id == FDB_OBJECT_MAIN)
    {
        msg->context(mContext);
        return this;
    }
    
    CFdbBaseObject *object = 0;
    bool tried_to_create = false;

    while (1)
    {
        object = findObject(obj_id, server_only);
        if (object)
        {
            msg->context(object->endpoint()->context());
            break;
        }
        else if (tried_to_create)
        {
            break;
        }
        
        onCreateObject(this, msg);
        tried_to_create = true;
    }

    return object;
}

bool CBaseEndpoint::addConnectedSession(CFdbSessionContainer *socket, CFdbSession *session)
{
    if (!authentication(session))
    {
        return false;
    }

    bool is_first;
    if (onlineSeparateChannel())
    {   // separate last session flag between secure and insecure channel for client
        if (session->isSecure())
        {
            is_first = !mSecureSessionCnt;
        }
        else
        {
            is_first = !mInsecureSessionCnt;
        }
    }
    else
    {   // last session flag of total channels for server
        is_first = !(mSecureSessionCnt + mInsecureSessionCnt);
    }

    if (session->isSecure())
    {
        mSecureSessionCnt++;
    }
    else
    {
        mInsecureSessionCnt++;
    }

    socket->addSession(session);
    registerSession(session);
    notifyOnline(session, is_first);

    auto &object_tbl = mObjectContainer.getContainer();
    if (!object_tbl.empty())
    {
        // get a snapshot of the table to avoid modification of the table in callback
        auto object_tbl = mObjectContainer.getContainer();
        for (auto it = object_tbl.begin(); it != object_tbl.end(); ++it)
        {
            auto object = it->second;
            if (!object->authentication(session))
            {
                continue;
            }
            object->notifyOnline(session, is_first);
        }
    }
    
    return true;
}

void CBaseEndpoint::deleteConnectedSession(CFdbSession *session)
{
    bool is_last;
    
    if (onlineSeparateChannel())
    {   // separate last session flag between secure and insecure channel for client
        if (session->isSecure())
        {
            is_last = (mSecureSessionCnt == 1);
        }
        else
        {
            is_last = (mInsecureSessionCnt == 1);
        }
    }
    else
    {   // last session flag of total channels for server
        is_last = (mSecureSessionCnt + mInsecureSessionCnt) == 1;
    }

    if (session->isSecure())
    {
        if (mSecureSessionCnt > 0)
        {
            mSecureSessionCnt--;
        }
        else
        {
            LOG_E("CBaseEndpoint: secure session count < 0 for object %s!\n", mName.c_str());
        }
    }
    else
    {
        if (mInsecureSessionCnt > 0)
        {
            mInsecureSessionCnt--;
        }
        else
        {
            LOG_E("CBaseEndpoint: secure session count < 0 for object %s!\n", mName.c_str());
        }
    }
    
    session->container()->removeSession(session);

    auto &object_tbl = mObjectContainer.getContainer();
    if (!object_tbl.empty())
    {
        // get a snapshot of the table to avoid modification of the table in callback
        auto object_tbl = mObjectContainer.getContainer();
        for (auto it = object_tbl.begin(); it != object_tbl.end(); ++it)
        {
            CFdbBaseObject *object = it->second;
            object->notifyOffline(session, is_last);
        }
    }

    notifyOffline(session, is_last);
    unregisterSession(session->sid());
}

CFdbBaseObject *CBaseEndpoint::findObject(FdbObjectId_t obj_id, bool server_only)
{
    auto &object_tbl = mObjectContainer.getContainer();
    for (auto it = object_tbl.begin(); it != object_tbl.end(); ++it)
    {
        auto object = it->second;
        if (server_only)
        {
            if ((object->role() == FDB_OBJECT_ROLE_SERVER) &&
                (FDB_OBJECT_GET_CLASS(obj_id) == 
                    FDB_OBJECT_GET_CLASS(object->objId())))
            {
                return object;
            }
        }
        else if (obj_id == object->objId())
        {
            return object;
        }
    }
    return 0;
}

//================================== destroy ==========================================
class CDestroyJob : public CMethodJob<CBaseEndpoint>
{
public:
    CDestroyJob(CBaseEndpoint *object, bool prepare)
        : CMethodJob<CBaseEndpoint>(object, &CBaseEndpoint::callDestroyEndpoint, JOB_FORCE_RUN)
        , mPrepare(prepare)
    {
    }
    bool mPrepare;
};

void CBaseEndpoint::callDestroyEndpoint(CMethodJob<CBaseEndpoint> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CDestroyJob *>(job);
    if (the_job->mPrepare)
    {
        auto &object_tbl = mObjectContainer.getContainer();
        while (!object_tbl.empty())
        {
            auto it = object_tbl.begin();
            CFdbBaseObject *object = it->second;
            removeObject(object);
        }

        mContext->doUnregisterEndpoint(this);
    }
    else
    {
        deleteSocket();
#ifdef CONFIG_SSL
        if (mSSLContext)
        {
            SSL_CTX_free((SSL_CTX *)mSSLContext);
        }
#endif
        // ensure if prepare is not called, it still can be unregistered.
        mContext->doUnregisterEndpoint(this);
    }
}

void CBaseEndpoint::destroySelf(bool prepare)
{
    mContext->sendSyncEndeavor(new CDestroyJob(this, prepare));
}

bool CBaseEndpoint::importTokens(const CFdbParcelableArray<std::string> &in_tokens)
{
    auto need_update = false;

    if (in_tokens.size() != (uint32_t)mTokens.size())
    {
        need_update = true;
    }
    else
    {
        auto it = mTokens.begin();
        auto in_it = in_tokens.pool().begin();
         for (; in_it != in_tokens.pool().end(); ++in_it, ++it)
        {
            if (it->compare(*in_it))
            {
                need_update = true;
                break;
            }
        }
    }

    if (need_update)
    {
        mTokens.clear();
        for (auto in_it = in_tokens.pool().begin(); in_it != in_tokens.pool().end(); ++in_it)
        {
            mTokens.push_back(*in_it);
        }
    }
    return need_update;
}

int32_t CBaseEndpoint::checkSecurityLevel(const char *token)
{
    return CFdbToken::checkSecurityLevel(mTokens, token);
}

// given token in session, update security level
void CBaseEndpoint::updateSecurityLevel()
{
    auto &containers = getContainer();
    for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
    {
        auto container = socket_it->second;
        auto &tbl = container->mConnectedSessionTable;
        for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
        {
            auto session = *session_it;
            session->securityLevel(checkSecurityLevel(session->token().c_str()));
        }
    }
}

bool CBaseEndpoint::requestServiceAddress(const char *server_name, FdbInstanceId_t instance_id)
{
    if (role() == FDB_OBJECT_ROLE_NS_SERVER)
    {
        return false;
    }
    if (server_name)
    {
        setNsName(server_name);
        setInstanceId(instance_id);
    }
    if (mNsName.empty())
    {
        // server name is not ready: this might happen when name server
        // is connected but bind() or connect() is not called.
        return false;
    }

    auto name_proxy = FDB_CONTEXT->getNameProxy();
    if (!name_proxy)
    {
        return false;
    }

    if (role() == FDB_OBJECT_ROLE_SERVER)
    {
        if (mContext->serverAlreadyRegistered(this))
        {
            LOG_E("CBaseEndpoint: service %s, instance: %d exists and fails to bind again!\n",
                    mNsName.c_str(), mInstanceId);
            return false;
        }
        name_proxy->addAddressListener(mInstanceId, mNsName.c_str());
        name_proxy->registerService(this);
    }
    else
    {
        name_proxy->listenOnService(this);
    }
    mEventRouter.connectPeers();
    return true;
}

bool CBaseEndpoint::releaseServiceAddress()
{
    if (role() == FDB_OBJECT_ROLE_NS_SERVER)
    {
        return false;
    }
    if (mNsName.empty())
    {
        // server name is not ready: this might happen when name server
        // is connected but bind() or connect() is not called.
        return false;
    }

    auto name_proxy = FDB_CONTEXT->getNameProxy();
    if (!name_proxy)
    {
        return false;
    }

    if (role() == FDB_OBJECT_ROLE_SERVER)
    {
        name_proxy->removeAddressListener(mInstanceId, mNsName.c_str());
        name_proxy->unregisterService(this);
    }
    else
    {
        name_proxy->removeServiceListener(mInstanceId, mNsName.c_str());
    }
    mNsName.clear();
    return true;
}

void CBaseEndpoint::onSidebandInvoke(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    switch (msg->code())
    {
        case FDB_SIDEBAND_SESSION_INFO:
        {
            FdbSessionInfo sinfo;
            CFdbParcelableParser parser(sinfo);
            if (!msg->deserialize(parser))
            {
                return;
            }
            auto session = msg->getSession();
            if (!session)
            {
                return;
            }
            session->senderName(sinfo.sender_name().c_str());
            session->pid((CBASE_tProcId)sinfo.pid());
            std::string peer_ip;
            int32_t udp_port = FDB_INET_PORT_INVALID;
            if (sinfo.has_udp_port())
            {
                udp_port = sinfo.udp_port();
            }
            if (FDB_VALID_PORT(udp_port) && session->peerIp(peer_ip))
            {
                session->setUDPDestAddr(peer_ip.c_str(), udp_port);
            }
        }
        break;
        case FDB_SIDEBAND_QUERY_SESSION_INFO:
        {
            FdbMsgClientTable clt_tbl;
            clt_tbl.set_endpoint_name(name().c_str());
            clt_tbl.set_server_name(nsName().c_str());
            clt_tbl.set_instance_id(instanceId());
            if (mContext)
            {
                clt_tbl.set_context_job_queue_size(mContext->jobQueueSize(false) + mContext->jobQueueSize(true));
            }
            if (mWorker)
            {
                clt_tbl.set_worker_job_queue_size(mWorker->jobQueueSize(false) + mWorker->jobQueueSize(true));
            }
            auto &containers = getContainer();
            for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
            {
                auto container = socket_it->second;
                if (!container->mConnectedSessionTable.empty())
                {
                    // get a snapshot of the table to avoid modification of the table in callback
                    auto tbl = container->mConnectedSessionTable;
                    for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
                    {
                        auto session = *session_it;
                        CFdbSessionInfo sinfo;
                        session->getSessionInfo(sinfo);
                        auto cinfo = clt_tbl.add_client_tbl();
                        cinfo->set_peer_name(session->senderName().c_str());
                        std::string addr;
                        if (sinfo.mContainerSocket.mAddress->mType == FDB_SOCKET_IPC)
                        {
                            addr = sinfo.mContainerSocket.mAddress->mAddr.c_str();
                        }
                        else
                        {
                            addr = sinfo.mConn->mPeerIp + ":" +
                                   std::to_string(sinfo.mConn->mPeerPort);
                        }
                        cinfo->set_peer_address(addr.c_str());
                        cinfo->set_security_level(session->securityLevel());

                        int32_t peer_udp_port;
                        session->getUDPDestAddr(peer_udp_port);
                        cinfo->set_udp_port(peer_udp_port);
                        int32_t dog_status = mWatchdog ?
                                    mWatchdog->queryDog(session) : FDB_DOG_ST_NON_EXIST;
                        cinfo->set_dog_status((FdbMsgDogStatus)dog_status);
                        cinfo->set_pid(session->pid());
                        cinfo->set_sid(session->sid());

                        auto &session_tcp_tx_statics = session->getStatistics().mTx;
                        auto &msg_tcp_tx_statics = cinfo->tcp_tx_statistics();
                        msg_tcp_tx_statics.mSyncRequest     = session_tcp_tx_statics.mSyncRequest;
                        msg_tcp_tx_statics.mSyncReply       = session_tcp_tx_statics.mSyncReply;
                        msg_tcp_tx_statics.mAsyncRequest    = session_tcp_tx_statics.mAsyncRequest;
                        msg_tcp_tx_statics.mAsyncReply      = session_tcp_tx_statics.mAsyncReply;
                        msg_tcp_tx_statics.mBroadcast       = session_tcp_tx_statics.mBroadcast;
                        msg_tcp_tx_statics.mPublish         = session_tcp_tx_statics.mPublish;
                        msg_tcp_tx_statics.mSend            = session_tcp_tx_statics.mSend;
                        msg_tcp_tx_statics.mGetEvent        = session_tcp_tx_statics.mGetEvent;
                        msg_tcp_tx_statics.mReturnEvent     = session_tcp_tx_statics.mReturnEvent;
                        msg_tcp_tx_statics.mSyncStatus      = session_tcp_tx_statics.mSyncStatus;
                        msg_tcp_tx_statics.mAsyncStatus     = session_tcp_tx_statics.mAsyncStatus;

                        auto &session_tcp_rx_statics = session->getStatistics().mRx;
                        auto &msg_tcp_rx_statics = cinfo->tcp_rx_statistics();
                        msg_tcp_rx_statics.mSyncRequest     = session_tcp_rx_statics.mSyncRequest;
                        msg_tcp_rx_statics.mSyncReply       = session_tcp_rx_statics.mSyncReply;
                        msg_tcp_rx_statics.mAsyncRequest    = session_tcp_rx_statics.mAsyncRequest;
                        msg_tcp_rx_statics.mAsyncReply      = session_tcp_rx_statics.mAsyncReply;
                        msg_tcp_rx_statics.mBroadcast       = session_tcp_rx_statics.mBroadcast;
                        msg_tcp_rx_statics.mPublish         = session_tcp_rx_statics.mPublish;
                        msg_tcp_rx_statics.mSend            = session_tcp_rx_statics.mSend;
                        msg_tcp_rx_statics.mGetEvent        = session_tcp_rx_statics.mGetEvent;
                        msg_tcp_rx_statics.mReturnEvent     = session_tcp_rx_statics.mReturnEvent;
                        msg_tcp_rx_statics.mSyncStatus      = session_tcp_rx_statics.mSyncStatus;
                        msg_tcp_rx_statics.mAsyncStatus     = session_tcp_rx_statics.mAsyncStatus;

                        cinfo->set_qos(session->qos());
                        cinfo->set_tx_queue_size(session->getPendingChunkSize());
                    }
                }
            }
            CFdbParcelableBuilder builder(clt_tbl);
            msg->replySideband(msg_ref, builder);
        }
        break;
        case FDB_SIDEBAND_QUERY_PEER_SESSION_INFO:
        {
            FdbMsgQueryPeerInfo query;
            CFdbParcelableParser parser(query);
            if (!msg->deserialize(parser))
            {
                return;
            }
            auto session = getSession(query.sid());
            if (session)
            {
                invokeSideband(query.sid(), new CQueryPeerSessionMsg(FDB_SIDEBAND_QUERY_SESSION_INFO,
                                                                     msg_ref, session->qos()), 0, 0,
                                                                     FDB_QUERY_MAX_TIMEOUT);
            }
            else
            {
                msg->statusf(msg_ref, FDB_ST_NON_EXIST, "Session %d doesn't exist!", query.sid());
            }
        }
        break;
        default:
            CFdbBaseObject::onSidebandInvoke(msg_ref);
        break;
    }
}

void CBaseEndpoint::onSidebandReply(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    switch (msg->code())
    {
        case FDB_SIDEBAND_QUERY_SESSION_INFO:
        {
            auto query_msg = castToMessage<CQueryPeerSessionMsg *>(msg_ref);
            if (query_msg->isStatus())
            {
                int32_t id;
                std::string reason;
                query_msg->decodeStatus(id, reason);
                auto reply_msg = castToMessage<CFdbMessage *>(query_msg->mReq);
                reply_msg->status(query_msg->mReq, id, reason.c_str());
                return;
            }

            auto reply_msg = castToMessage<CFdbMessage *>(query_msg->mReq);
            reply_msg->reply(query_msg->mReq, query_msg->getPayloadBuffer(), query_msg->getPayloadSize());
        }
        break;
        default:
            CFdbBaseObject::onSidebandReply(msg_ref);
        break;
    }
}

void CBaseEndpoint::updateSessionInfo(CFdbSession *session)
{
    if ((role() != FDB_OBJECT_ROLE_SERVER) && (role() != FDB_OBJECT_ROLE_CLIENT))
    {
        return;
    }

    CFdbSessionInfo sinfo_connected;
    session->getSessionInfo(sinfo_connected);

    int32_t udp_port = FDB_INET_PORT_INVALID;
    if (sinfo_connected.mContainerSocket.mAddress->mType == FDB_SOCKET_TCP)
    {
        if (role() == FDB_OBJECT_ROLE_CLIENT)
        {
            // for client, self IP address is unknown until session is connected.
            // for server, UDP is created upon binding because IP address of UDP
            //  is the same as that of TCP socket
            session->container()->bindUDPSocket(sinfo_connected.mConn->mSelfAddress.mAddr.c_str());
        }
        else if (!session->container()->getUDPSession())
        {   // if udp session is destroyed due to CFdbUDPSession::onError(),
            // rebind to the UDP port.
            session->container()->bindUDPSocket(0);
        }

        CFdbSocketInfo socket_info;
        if (session->container()->getUDPSocketInfo(socket_info) &&
            FDB_VALID_PORT(socket_info.mAddress->mPort))
        {
            udp_port = socket_info.mAddress->mPort;
        }
    }
    FdbSessionInfo sinfo_sent;
    sinfo_sent.set_sender_name(mName.c_str());
    sinfo_sent.set_pid((uint32_t)CBaseThread::getPid());
    if (FDB_VALID_PORT(udp_port))
    {
        sinfo_sent.set_udp_port(udp_port);
    }
    CFdbParcelableBuilder builder(sinfo_sent);
    sendSideband(session, FDB_SIDEBAND_SESSION_INFO, builder);
}

CFdbSession *CBaseEndpoint::connectedSession(const CFdbSocketAddr &addr)
{
    auto &containers = getContainer();
    for (auto it = containers.begin(); it != containers.end(); ++it)
    {
        auto container = it->second;
        auto session = container->connectedSession(addr);
        if (session)
        {
            return session;
        }
    }
    return 0;
}

CFdbSession *CBaseEndpoint::connectedSession(EFdbSecureType sec_type)
{
    auto &containers = getContainer();
    for (auto it = containers.begin(); it != containers.end(); ++it)
    {
        auto container = it->second;
        auto session = container->connectedSession(sec_type);
        if (session)
        {
            return session;
        }
    }
    return 0;
}

CFdbSession *CBaseEndpoint::bound(const CFdbSocketAddr &addr)
{
    auto &containers = getContainer();
    for (auto it = containers.begin(); it != containers.end(); ++it)
    {
        auto container = it->second;
        auto session = container->bound(addr);
        if (session)
        {
            return session;
        }
    }
    return 0;
}

bool CBaseEndpoint::onMessageAuthentication(CFdbMessage *msg, CBaseSession *session)
{
    const CApiSecurityConfig *sec_cfg = getApiSecurityConfig();
    if (sec_cfg)
    {
        auto required_security_level = sec_cfg->getMessageSecLevel(msg->code());
        auto actual_security_level = session->securityLevel();
        if (actual_security_level == FDB_SECURITY_LEVEL_UNKNOWN)
        {
            actual_security_level = session->checkSecurityLevel();
        }
        return actual_security_level >= required_security_level;
    }
    return true;
}

bool CBaseEndpoint::onGetEventAuthentication(CFdbMessage *msg, CBaseSession *session)
{
    const CApiSecurityConfig *sec_cfg = getApiSecurityConfig();
    if (sec_cfg)
    {
        auto required_security_level = sec_cfg->getEventSecLevel(msg->code());
        auto actual_security_level = session->securityLevel();
        if (actual_security_level == FDB_SECURITY_LEVEL_UNKNOWN)
        {
            actual_security_level = session->checkSecurityLevel();
        }
        return actual_security_level >= required_security_level;
    }
    return true;
}

bool CBaseEndpoint::onSetEventAuthentication(CFdbMessage *msg, CBaseSession *session)
{
    return true;
}

bool CBaseEndpoint::onPublishAuthentication(CFdbMessage *msg, CBaseSession *session)
{
    return true;
}

void CBaseEndpoint::onPublish(CBaseJob::Ptr &msg_ref)
{
    CFdbBaseObject::onPublish(msg_ref);
    auto msg = castToMessage<CBaseMessage *>(msg_ref);
    auto session = getSession(msg->session());
    if (session)
    {
        mEventRouter.routeMessage(session, msg);
    }
}

void CBaseEndpoint::registerSession(CFdbSession *session)
{
    auto sid = mSessionContainer.allocateEntityId();
    session->sid(sid);
    mSessionContainer.insertEntry(sid, session);
}

CFdbSession *CBaseEndpoint::getSession(FdbSessionId_t session_id)
{
    CFdbSession *session = 0;
    mSessionContainer.retrieveEntry(session_id, session);
    return session;
}

void CBaseEndpoint::unregisterSession(FdbSessionId_t session_id)
{
    CFdbSession *session = 0;
    auto it = mSessionContainer.retrieveEntry(session_id, session);
    if (session)
    {
        mSessionContainer.deleteEntry(it);
    }
}

void CBaseEndpoint::deleteSession(FdbSessionId_t session_id)
{
    CFdbSession *session = 0;
    (void)mSessionContainer.retrieveEntry(session_id, session);
    if (session)
    {
        delete session;
    }
}

void CBaseEndpoint::deleteSession(CFdbSessionContainer *container)
{
    auto &session_tbl = mSessionContainer.getContainer();
    for (auto it = session_tbl.begin(); it != session_tbl.end();)
    {
        CFdbSession *session = it->second;
        ++it;
        if (session->container() == container)
        {
            delete session;
        }
    }
}

#ifdef CONFIG_SSL
bool CBaseEndpoint::createSSL(const char *public_key,
                              const char *private_key,
                              const char *root_ca,
                              uint32_t verify_type)
{
    void *ssl_context = CBaseSocketFactory::createSSL(public_key,
                                                      private_key,
                                                      root_ca,
                                                      verify_type,
                                                      role() == FDB_OBJECT_ROLE_CLIENT);
    if (!ssl_context)
    {
        return false;
    }
    if (mSSLContext)
    {
        SSL_CTX_free((SSL_CTX *)mSSLContext);
    }
    mSSLContext = ssl_context;
    return true;
}

bool CBaseEndpoint::createSSL(const char *public_key,
                              const char *private_key,
                              const char *root_ca)
{
    return createSSL(public_key, private_key, root_ca, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
}

bool CBaseEndpoint::createSSL(const char *public_key,
                              const char *private_key)
{
    return createSSL(public_key, private_key, 0, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
}

bool CBaseEndpoint::createSSL(const char *root_ca,
                              uint32_t verify_type)
{
    return createSSL(0, 0, root_ca, verify_type);
}

bool CBaseEndpoint::createSSL(const char *root_ca)
{
    return createSSL(0, 0, root_ca, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
}
#else
bool CBaseEndpoint::createSSL(const char *public_key,
                              const char *private_key,
                              const char *root_ca,
                              uint32_t verify_type)
{
    return false;
}
bool CBaseEndpoint::createSSL(const char *public_key,
                              const char *private_key,
                              const char *root_ca)
{
    return false;
}

bool CBaseEndpoint::createSSL(const char *public_key,
                              const char *private_key)
{
    return false;
}

bool CBaseEndpoint::createSSL(const char *root_ca,
                              uint32_t verify_type)
{
  return false;
}

bool CBaseEndpoint::createSSL(const char *root_ca)
{
  return false;
}
#endif

bool CBaseEndpoint::checkOnlineChannel(EFdbSecureType type, const CFdbOnlineInfo &channel_info)
{
    if ((type == FDB_SEC_NO_CHECK) || (channel_info.mQOS == FDB_QOS_LOCAL))
    {
        return true;
    }

    bool allowed = false;
    if (type == FDB_SEC_TRY_SECURE)
    {
        type = TCPSecureEnabled() ? FDB_SEC_SECURE : FDB_SEC_INSECURE;
    }
    if (type == FDB_SEC_INSECURE)
    {
        if (channel_info.mQOS == FDB_QOS_RELIABLE)
        {
            allowed = true;
        }
    }
    else
    {
        if (channel_info.mQOS == FDB_QOS_SECURE_RELIABLE)
        {
            allowed = true;
        }
    }

    return allowed;
}

void CBaseEndpoint::regularHouseKeeping()
{
    auto &containers = getContainer();
    for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
    {
        auto container = socket_it->second;
        auto tbl = container->mConnectedSessionTable;
        for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
        {
            auto session = *session_it;
            session->scanLifeTime();
        }
    }
}

void CBaseEndpoint::setContext(CFdbBaseContext *ctx)
{
    if (!ctx)
    {
        ctx = FDB_CONTEXT;
    }
    if (mContext == ctx)
    {
        return;
    }
    if (mContext)
    {
        mContext->unregisterEndpoint(this);
        mContext = 0;
    }
    if (!mContext)
    {
        mContext = ctx;
    }

    mContext->registerEndpoint(this);
}
}
}
