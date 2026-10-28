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

#include "CInterNameProxy.h"
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CFdbSession.h>
#include "CIntraHostProxy.h"
#include "CNameServer.h"
#include "CSvcAddrUtils.h"
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
#define FDB_MSG_TYPE_SUBSCRIBE (FDB_MSG_TYPE_SYSTEM + 1)
class CServiceSubscribeMsg : public CFdbMessage
{
public:
    CServiceSubscribeMsg(CBaseJob::Ptr &msg_ref)
        : CFdbMessage()
        , mMsgRef(msg_ref)
    {
    }
    CBaseJob::Ptr &getMsgRef()
    {
        return mMsgRef;
    }
    FdbMessageType_t getTypeId()
    {
        return FDB_MSG_TYPE_SUBSCRIBE;
    }
private:
    CBaseJob::Ptr mMsgRef;
};

CQueryServiceMsg::CQueryServiceMsg(tPendingServiceReqTbl *pending_tbl,
                                    FdbMsgServiceTable *svc_tbl,
                                    CBaseJob::Ptr &req,
                                    const std::string &host_ip,
                                    CNameServer *name_server)
    : CFdbMessage(REQ_QUERY_SERVICE_INTER_MACHINE)
    , mPendingReqTbl(pending_tbl)
    , mSvcTbl(svc_tbl)
    , mReq(req)
    , mHostIp(host_ip)
    , mNameServer(name_server)
{
}

CQueryServiceMsg::~CQueryServiceMsg()
{
}

void CQueryServiceMsg::onAsyncError(Ptr &ref, FdbMsgStatusCode code, const char *reason)
{
    mNameServer->finalizeServiceQuery(0, this);
}

CInterNameProxy::ConnectAddrSubscriber::ConnectAddrSubscriber(CInterNameProxy *ns_proxy, SubscribeType type,
                                                              const char *name)
    : CFdbBaseObject(name)
    , mNsProxy(ns_proxy)
    , mType(type)
{
}

void CInterNameProxy::ConnectAddrSubscriber::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgAddressList msg_addr_list;
    CFdbParcelableParser parser(msg_addr_list);
    if (!msg->deserialize(parser))
    {
        return;
    }

    mNsProxy->onServiceBroadcast(msg, mType);
}

void CInterNameProxy::ConnectAddrSubscriber::connectWithProxy()
{
    // type is regarded as object ID
    connect(mNsProxy, mType);
}

CInterNameProxy::CInterNameProxy(CNameProxyContainer *container,
                                 const char *host_ip,
                                 const char *ns_url,
                                 const char *host_name)
    : mContainer(container)
    , mIpAddress(host_ip)
    , mHostName(host_name)
    , mInterNormalSubscriber(this, INTER_NORMAL, "SVC subscriber")
    , mInterMonitorSubscriber(this, INTER_MONITOR, "SVC monitor")
{
    mName = mContainer->nameServer()->hostName();
    mName += "(remote)";

    mNsUrl = ns_url;

    mInterNormalSubscriber.connectWithProxy();
    mInterMonitorSubscriber.connectWithProxy();
}

CInterNameProxy::~CInterNameProxy()
{
    mContainer->deleteNameProxy(this);
}

void CInterNameProxy::addServiceListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mInterNormalSubscriber.subscribe(subscribe_list);
    }
}

void CInterNameProxy::addServiceListener(FdbInstanceId_t instance_id, const char *svc_name,
                                         CBaseJob::Ptr &msg_ref)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        auto msg = new CServiceSubscribeMsg(msg_ref);
        mInterNormalSubscriber.subscribe(subscribe_list, msg);
    }
}

void CInterNameProxy::removeServiceListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mInterNormalSubscriber.unsubscribe(subscribe_list);
    }
} 

void CInterNameProxy::addServiceMonitorListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mInterMonitorSubscriber.subscribe(subscribe_list);
    }
}

void CInterNameProxy::addServiceMonitorListener(FdbInstanceId_t instance_id, const char *svc_name,
                                                CBaseJob::Ptr &msg_ref)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        auto msg = new CServiceSubscribeMsg(msg_ref);
        mInterMonitorSubscriber.subscribe(subscribe_list, msg);
    }
}

void CInterNameProxy::removeServiceMonitorListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mInterMonitorSubscriber.unsubscribe(subscribe_list);
    }
}

void CInterNameProxy::validateUrl(FdbMsgAddressList &msg_addr_list, CFdbSession *session)
{
    CSvcAddrUtils::validateUrl(msg_addr_list, session);
}

void CInterNameProxy::onServiceBroadcast(CFdbMessage *msg, SubscribeType subscribe_type)
{
    auto name_server = mContainer->nameServer();
    auto forward_type = subscribe_type == INTER_NORMAL ? INTRA_NORMAL : INTRA_MONITOR;
    FdbMsgAddressList msg_addr_list;
    CFdbParcelableParser parser(msg_addr_list);
    if (!msg->deserialize(parser))
    {
        return;
    }

    tSubscribedSessionSets sessions;
    auto svc_name = msg_addr_list.service_name().c_str();
    mContainer->nameServer()->getSvcSubscribeTable(msg->code(), svc_name, sessions, forward_type);
    if (sessions.empty())
    {
        // if no client is connected to the server, unsubscrie it from
        // name server of other hosts.
        name_server->recallServiceListener(msg->code(), svc_name, subscribe_type);
        return;
    }

    // now show forward appear/disappear of the server to local clients
    auto *session = msg->getSession();
    validateUrl(msg_addr_list, session);

    CFdbMessage *initial_sub_msg = 0;
    if (session)
    {
        auto out_going_msg = session->peepPendingMessage(msg->sn());
        if (out_going_msg && (out_going_msg->getTypeId() == FDB_MSG_TYPE_SUBSCRIBE))
        {
            auto second_sub_msg = fdb_dynamic_cast_if_available<CServiceSubscribeMsg *>(out_going_msg);
            initial_sub_msg = castToMessage<CFdbMessage *>(second_sub_msg->getMsgRef());
        }
    }

    if (forward_type == INTRA_NORMAL)
    {
        CFdbToken::tTokenList tokens;
        msg_addr_list.dumpTokens(tokens);
        msg_addr_list.token_list().clear_tokens();
        if (initial_sub_msg)
        {
            name_server->broadcastSvcAddrLocal(tokens, msg_addr_list, initial_sub_msg);
        }
        else
        {
            name_server->broadcastSvcAddrLocal(tokens, msg_addr_list);
        }
    }
    else
    {
        // never broadcast token to monitors!!!
        msg_addr_list.token_list().clear_tokens();
        CFdbParcelableBuilder builder(msg_addr_list);
        if (initial_sub_msg)
        {
            initial_sub_msg->broadcast(msg->code(), builder, svc_name);
        }
        else
        {
            name_server->broadcastService(msg->code(), builder, svc_name, forward_type);
        }
    }
}

bool CInterNameProxy::queryServiceTbl(CFdbMessage *msg)
{
    return connected() ? invoke(msg, 0, 0, FDB_QUERY_MAX_TIMEOUT) : false;
}

void CInterNameProxy::onReply(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (!msg)
    {
        return;
    }
    if (msg->isStatus())
    {
        int32_t id;
        std::string reason;
        msg->decodeStatus(id, reason);
        LOG_I("CInterNameProxy: status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());

        if (msg->code() == REQ_QUERY_SERVICE_INTER_MACHINE)
        {
            mContainer->nameServer()->finalizeServiceQuery(0, msg);
        }
        return;
    }

    switch (msg->code())
    {
        case REQ_QUERY_SERVICE_INTER_MACHINE:
        {
            FdbMsgServiceTable svc_tbl;
            CFdbParcelableParser parser(svc_tbl);
            if (msg->deserialize(parser))
            {
                mContainer->nameServer()->finalizeServiceQuery(&svc_tbl, msg);
            }
            else
            {
                mContainer->nameServer()->finalizeServiceQuery(0, msg);
            }
        }
        break;
        default:
        break;
    }
}

void CInterNameProxy::onOnline(const CFdbOnlineInfo &info)
{
    CBaseNameProxy::onOnline(info);
    mContainer->nameServer()->forwardServiceSubscription(this);
    LOG_I("CInterNameProxy: name server %s is connected.\n", mNsUrl.c_str());
}
void CInterNameProxy::onOffline(const CFdbOnlineInfo &info)
{
    CBaseNameProxy::onOffline(info);
}
}
}
