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

#include "CIntraNameProxy.h"
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CBaseServer.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CFdbSession.h>
#include <utils/CNsConfig.h>
#include <utils/Log.h>
#include <fdbus/CFdbWatchdog.h>

namespace ipc {
namespace fdbus {
#define FDB_MSG_TYPE_NSP_SUBSCRIBE (FDB_MSG_TYPE_SYSTEM - 1)

class CNSProxyMsg : public CFdbMessage
{
public:
    CNSProxyMsg(FdbContextId_t ctx_id, FdbEndpointId_t ep_id)
        : CFdbMessage()
        , mReqCtxId(ctx_id)
        , mReqEpId(ep_id)
    {
    }
    CNSProxyMsg(FdbMsgCode_t code, FdbContextId_t ctx_id, FdbEndpointId_t ep_id)
        : CFdbMessage(code)
        , mReqCtxId(ctx_id)
        , mReqEpId(ep_id)
    {
    }
    CNSProxyMsg(CFdbMessageHeader &head
                      , CBaseSession *session
                      , FdbContextId_t ctx_id
                      , FdbEndpointId_t ep_id)
        : CFdbMessage(head, session)
        , mReqCtxId(ctx_id)
        , mReqEpId(ep_id)
    {
    }
    FdbMessageType_t getTypeId()
    {
        return FDB_MSG_TYPE_NSP_SUBSCRIBE;
    }

    FdbContextId_t mReqCtxId;
    FdbEndpointId_t mReqEpId;
protected:
    CFdbMessage *clone(CFdbMessageHeader &head
                       , CBaseSession *session)
    {
        return new CNSProxyMsg(head, session, mReqCtxId, mReqEpId);
    }
};

CIntraNameProxy::ConnectAddrSubscriber::ConnectAddrSubscriber(CIntraNameProxy *ns_proxy, SubscribeType type,
                                                              const char *name)
    : CFdbBaseObject(name)
    , mNsProxy(ns_proxy)
    , mType(type)
{
}

void CIntraNameProxy::ConnectAddrSubscriber::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    if (mType == INTRA_NORMAL)
    {
        mNsProxy->onServiceBroadcast(msg_ref);
    }
    else if (mType == INTRA_MONITOR)
    {
        CFdbBaseObject::onBroadcast(msg_ref);
    }
}

void CIntraNameProxy::ConnectAddrSubscriber::connectWithProxy()
{
    // type is regarded as object ID
    connect(mNsProxy, mType);
}

CIntraNameProxy::BindAddrSubscriber::BindAddrSubscriber(CIntraNameProxy *ns_proxy, const char *name)
    : CFdbBaseObject(name)
    , mNsProxy(ns_proxy)
{
}

void CIntraNameProxy::BindAddrSubscriber::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);

    mNsProxy->bindAddress(msg, FDB_INVALID_ID, FDB_INVALID_ID);
}

void CIntraNameProxy::BindAddrSubscriber::connectWithProxy()
{
    // type is regarded as object ID
    bind(mNsProxy, MORE_ADDRESS);
}

CIntraNameProxy::CIntraNameProxy()
    : mNsWatchdogListener(0)
    , mIntraNormalSubscriber(this, INTRA_NORMAL, "SVC subscriber")
    , mIntraMonitorSubscriber(this, INTRA_MONITOR, "SVC monitor")
    , mBindAddrSubscriber(this, "Bind address Subscriber")
{
    mName  = std::to_string(CBaseThread::getPid());
    mName += "-nsproxy(local)";
#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
    mNsUrl = CNsConfig::getNameServerTCPUrl(TCPSecureEnabled());
#else
    mNsUrl = CNsConfig::getNameServerIPCUrl();
#endif
    worker(mContext);

    mIntraNormalSubscriber.connectWithProxy();
    mBindAddrSubscriber.connectWithProxy();
    mIntraMonitorSubscriber.connectWithProxy();
}

void CIntraNameProxy::listenOnService(FdbInstanceId_t instance_id, const char *svc_name,
                                      FdbContextId_t ctx_id, FdbEndpointId_t ep_id)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mIntraNormalSubscriber.subscribe(subscribe_list, new CNSProxyMsg(ctx_id, ep_id));
    }
}

void CIntraNameProxy::listenOnService(CBaseEndpoint *endpoint)
{
    listenOnService(endpoint->instanceId(), endpoint->nsName().c_str(),
                    endpoint->context()->ctxId(), endpoint->epid());
}

void CIntraNameProxy::removeServiceListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mIntraNormalSubscriber.unsubscribe(subscribe_list);
    }
}

void CIntraNameProxy::addAddressListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mBindAddrSubscriber.subscribe(subscribe_list);
    }
}

void CIntraNameProxy::removeAddressListener(FdbInstanceId_t instance_id, const char *svc_name)
{
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        subscribe_list.addNotifyItem(instance_id, svc_name);
        mBindAddrSubscriber.unsubscribe(subscribe_list);
    }
}

void CIntraNameProxy::registerService(CBaseEndpoint *endpoint)
{
    if (!connected())
    {
        return;
    }
    CFdbMessage *msg = new CNSProxyMsg(REQ_ALLOC_SERVICE_ADDRESS,
                                       endpoint->context()->ctxId(), endpoint->epid());
    FdbMsgServerName msg_svc_name;
    msg_svc_name.set_name(endpoint->nsName());
    msg_svc_name.set_endpoint_name(endpoint->name());
    msg_svc_name.set_allow_tcp_normal(endpoint->TCPEnabled());
    msg_svc_name.set_allow_tcp_secure(endpoint->TCPSecureEnabled());
    msg_svc_name.set_exportable_level(endpoint->exportableLevel());
    msg_svc_name.set_instance_id(endpoint->instanceId());
    CFdbParcelableBuilder builder(msg_svc_name);
    invoke(msg, builder);
}

void CIntraNameProxy::unregisterService(CBaseEndpoint *endpoint)
{
    if (!connected())
    {
        return;
    }
    FdbMsgServerName msg_svc_name;
    msg_svc_name.set_name(endpoint->nsName());
    msg_svc_name.set_instance_id(endpoint->instanceId());

    CFdbParcelableBuilder builder(msg_svc_name);
    send(REQ_UNREGISTER_SERVICE, builder);
}

void CIntraNameProxy::doConnectToAddress(CBaseClient *client,
                                         FdbMsgAddressList &msg_addr_list,
                                         bool &udp_failure,
                                         bool &udp_success,
                                         bool &tcp_failure)
{
    // for client, size of addr_list is always 1, which is ensured by name server
    auto &addr_list = msg_addr_list.address_list();
    const char *svc_name = msg_addr_list.service_name().c_str();
    const char *host_name = msg_addr_list.host_name().c_str();
    auto instance_id = msg_addr_list.instance_id();

    if (fdbIsGroup(client->instanceId()))
    {
        client->setInstanceId(instance_id);
    }
    if (msg_addr_list.has_token_list())
    {
        client->importTokens(msg_addr_list.token_list().tokens());
    }

    client->local(msg_addr_list.is_local());

    //TODO: put secure address before normal address 
    for (auto it = addr_list.vpool().begin(); it != addr_list.vpool().end(); ++it)
    {
        int32_t udp_port = FDB_INET_PORT_INVALID;
        if (it->has_udp_port())
        {
            udp_port = it->udp_port();
        }

        auto session_container = client->doConnect(it->tcp_ipc_url().c_str(),
                                                   host_name, udp_port);
        if (session_container)
        {
            session_container->setInterfaceId(it->interface_id());
            if (client->UDPEnabled() && (it->address_type() != FDB_SOCKET_IPC)
                && (udp_port > FDB_INET_PORT_NOBIND))
            {
                CFdbSocketInfo socket_info;
                if (!session_container->getUDPSocketInfo(socket_info) ||
                    (!FDB_VALID_PORT(socket_info.mAddress->mPort)))
                {
                    udp_failure = true;
                    LOG_I("CIntraNameProxy: Server: %s:%d, address %s UDP %d is connected but UDP fail.\n",
                          svc_name, instance_id, it->tcp_ipc_url().c_str(), udp_port);
                }
                else
                {
                    udp_success = true;
                    LOG_I("CIntraNameProxy: Server: %s:%d, address %s UDP %d is connected.\n",
                          svc_name, instance_id, it->tcp_ipc_url().c_str(), socket_info.mAddress->mPort);
                }
            }
        }
        else
        {
            tcp_failure = true;
            LOG_I("CIntraNameProxy: Server: %s:%d, address %s fail to connect TCP.\n",
                  svc_name, instance_id, it->tcp_ipc_url().c_str());
        }
    }
}

void CIntraNameProxy::doConnectToServer(CFdbBaseContext *context, FdbEndpointId_t ep_id,
                                        FdbMsgAddressList &msg_addr_list, bool is_init_response)
{
    auto svc_name = msg_addr_list.service_name().c_str();
    auto instance_id = msg_addr_list.instance_id();
    const std::string &host_name = msg_addr_list.host_name();
    bool is_offline = msg_addr_list.address_list().empty();
    int32_t udp_success_count = 0;
    int32_t udp_failure_count = 0;
    int32_t tcp_failure_count = 0;
    
    auto &container = context->getEndpoints().getContainer();
    for (auto ep_it = container.begin(); ep_it != container.end(); ++ep_it)
    {
        if (ep_it->second->role() != FDB_OBJECT_ROLE_CLIENT)
        {
            continue;
        }
        auto client = fdb_dynamic_cast_if_available<CBaseClient *>(ep_it->second);
        if (!client)
        {
            LOG_E("CIntraNameProxy: Fail to convert to CBaseEndpoint!\n");
            continue;
        }
        if (client->nsName().compare(svc_name))
        {
            continue;
        }
        else if (!fdbIsGroup(client->instanceId()) &&
                    (client->instanceId() != instance_id))
        {
            continue;
        }

        if (fdbValidFdbId(ep_id) && (ep_id != client->epid()))
        {
            continue;
        }

        if (is_offline)
        {
            if (client->hostConnected(host_name.c_str()))
            {
                // only disconnect the connected server (host name should match)
                client->doDisconnect();
                LOG_E("CIntraNameProxy Client %s:%d is disconnected by %s!\n", svc_name, instance_id, host_name.c_str());
            }
            else if (client->connected())
            {
                LOG_I("CIntraNameProxy: Client %s:%d ignore disconnect from %s.\n", svc_name, instance_id, host_name.c_str());
            }
        }
        else
        {
            CConnectionInfo conn_info = {&msg_addr_list.service_name(),
                                         instance_id,
                                         &msg_addr_list.endpoint_name(),
                                         &msg_addr_list.host_name(),
                                         &msg_addr_list.domain_name(),
                                         msg_addr_list.is_local()};
            if (client->connectionEnabled(conn_info))
            {
                bool udp_failure = false;
                bool udp_success = false;
                bool tcp_failure = false;
                doConnectToAddress(client, msg_addr_list, udp_failure, udp_success, tcp_failure);
                if (udp_failure)
                {
                    udp_failure_count++;
                }
                if (udp_success)
                {
                    udp_success_count++;
                }
                if (tcp_failure)
                {
                    tcp_failure_count++;
                }
            }
        }
    }
    if (!is_offline && udp_failure_count)
    {
        // msg->isInitialResponse() true: client starts after server; UDP port will be
        //                                allocated for each client
        //                          false: client starts before server; only one UDP port
        //                                is allocated. Subsequent allocation shall be
        //                                triggered manually
        // if client starts before server, once server appears, only one event is
        // received by client process. If more than one client connects to the same
        // server in the same process, shall ask name server to allocate UDP port
        // for each client. A calling to listenOnService() will lead to a
        // broadcast of server address with isInitialResponse() being true. Only
        // UDP port will be valid and TCP address will be discussed in subsequent broadcast.
        int32_t nr_request = 0;
        if (!is_init_response)
        {   // client starts before server and at least one fails to bind UDP: In this case
            // we should send requests of the same number as failures in the hope that we
            // can get enough response with new UDP port ID
            nr_request = udp_failure_count;
        }
        else if (!(is_init_response && udp_success_count))
        {   // client starts after server and none success to bind UDP: in this case only
            // one request is needed since there are several other requesting on-going
            nr_request = 1;
        }
        if (tcp_failure_count && !nr_request)
        {
            nr_request = 1;
        }
        for (int32_t i = 0; i < nr_request; ++i)
        {
            LOG_E("CIntraNameProxy: Server: %s: requesting next UDP...\n", svc_name);
            listenOnService(instance_id, svc_name, context->ctxId(), ep_id);
        }
    }
}

void CIntraNameProxy::onServiceBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbContextId_t ctx_id = FDB_INVALID_ID;
    FdbEndpointId_t ep_id = FDB_INVALID_ID;
    if (msg->isInitialResponse() && (msg->getTypeId() == FDB_MSG_TYPE_NSP_SUBSCRIBE))
    {
        auto nsp_msg = fdb_dynamic_cast_if_available<CNSProxyMsg *>(msg);
        ctx_id = nsp_msg->mReqCtxId;
        ep_id = nsp_msg->mReqEpId;
    }
    connectToServer(msg, ctx_id, ep_id);
}

void CIntraNameProxy::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    switch (msg->code())
    {
        case NTF_HOST_INFO:
        {
            FdbMsgHostInfo msg_host_info;
            CFdbParcelableParser parser(msg_host_info);
            if (!msg->deserialize(parser))
            {
                return;
            }
            mHostName = msg_host_info.name();
            CHostNameReady name_ready(mHostName);
            mHostNameNtfCenter.notify(name_ready);
        }
        break;
        case NTF_WATCHDOG:
        {
            if (!mNsWatchdogListener)
            {
                return;
            }
            CFdbMsgProcessList msg_process_list;
            CFdbParcelableParser parser(msg_process_list);
            if (!msg->deserialize(parser))
            {
                return;
            }
            auto &process_list = msg_process_list.process_list();
            tNsWatchdogList output_process_list;
            for (auto it = process_list.vpool().begin(); it != process_list.vpool().end(); ++it)
            {
                CNsWatchdogItem item(it->client_name().c_str(), it->pid());
                output_process_list.push_back(std::move(item));
            }
            mNsWatchdogListener(output_process_list);
        }
        break;
        default:
        break;
    }
}

void CIntraNameProxy::doBindAddress(CFdbBaseContext *context, FdbEndpointId_t ep_id,
                                    FdbMsgAddressList &msg_addr_list, bool force_rebind)
{
    auto svc_name = msg_addr_list.service_name().c_str();
    auto instance_id = msg_addr_list.instance_id();
    FdbMsgAddrBindResults bound_list;

    bound_list.set_service_name(msg_addr_list.service_name());
    bound_list.set_instance_id(instance_id);
    auto &container = context->getEndpoints().getContainer();
    for (auto ep_it = container.begin(); ep_it != container.end(); ++ep_it)
    {
        if (ep_it->second->role() != FDB_OBJECT_ROLE_SERVER)
        {
            continue;
        }
        auto server = fdb_dynamic_cast_if_available<CBaseServer *>(ep_it->second);
        if (server->nsName().compare(svc_name))
        {
            continue;
        }
        else if (!fdbIsGroup(server->instanceId()) &&
                    (server->instanceId() != instance_id))
        {
            continue;
        }

        if (fdbValidFdbId(ep_id))
        {
            if (ep_id == server->epid())
            {   // indicate that we've own a valid service name
                server->validSvcName(true);
            }
            else
            {
                continue;
            }
        }
        else if (!server->validSvcName())
        {   // We come here since address is broadcasted from name server
            // If the server doesn't own a name yet, skip it.
            LOG_E("CIntraNameProxy: Unable to bind for server %s - %s since it doesn't own a service name!\n",
                    server->name().c_str(), server->nsName().c_str());
            continue;
        }

        if (fdbIsGroup(server->instanceId()))
        {
            server->setInstanceId(instance_id);
        }

        if (msg_addr_list.has_token_list() && server->importTokens(msg_addr_list.token_list().tokens()))
        {
            server->updateSecurityLevel();
        }

        auto &addr_list = msg_addr_list.address_list();
        if (force_rebind)
        {
            server->doUnbind();
        }
        for (auto it = addr_list.vpool().begin(); it != addr_list.vpool().end(); ++it)
        {
            auto *addr_status = bound_list.add_address_list();
            const std::string &tcp_ipc_url = it->tcp_ipc_url();
            addr_status->request_address(tcp_ipc_url);
            int32_t udp_port = FDB_INET_PORT_INVALID;
            if (it->has_udp_port())
            {
                udp_port = it->udp_port();
            }
            CServerSocket *sk = server->doBind(tcp_ipc_url.c_str(), udp_port);
            if (!sk)
            {
                continue;
            }
            sk->setInterfaceId(it->interface_id());
            int32_t bound_port = FDB_INET_PORT_INVALID;
            CFdbSocketInfo info;
            std::string url;
            auto char_url = tcp_ipc_url.c_str();
            sk->getSocketInfo(info);
            if (it->address_type() == FDB_SOCKET_TCP)
            {
                if (it->tcp_port() == info.mAddress->mPort)
                {
                    addr_status->bind_address(tcp_ipc_url);
                }
                else
                {
                    if (it->tcp_port())
                    {
                        LOG_W("CIntraNameProxy: Server: %s, port %d is intended but get %d.\n",
                               svc_name, it->tcp_port(), info.mAddress->mPort);
                    }
                    CBaseSocketFactory::buildUrl(url, it->tcp_ipc_address().c_str(), info.mAddress->mPort,
                                                 it->is_secure());
                    addr_status->bind_address(url);
                    char_url = url.c_str();
                }
                if (server->UDPEnabled())
                {
                    CFdbSocketInfo socket_info;
                    if (sk->getUDPSocketInfo(socket_info) && FDB_VALID_PORT(socket_info.mAddress->mPort))
                    {
                        bound_port = socket_info.mAddress->mPort;
                    }
                }
            }
            else
            {
                addr_status->bind_address(tcp_ipc_url);
            }
            addr_status->set_udp_port(bound_port);

            if (FDB_VALID_PORT(bound_port))
            {
                LOG_I("CIntraNameProxy: Server: %s:%d, address %s UDP %d is bound.\n",
                      svc_name, instance_id, char_url, bound_port);
            }
            else
            {
                LOG_I("CIntraNameProxy: Server: %s:%d, address %s is bound.\n",
                      svc_name, instance_id, char_url);
            }
        }
        CFdbParcelableBuilder builder(bound_list);
        send(REQ_REGISTER_SERVICE, builder);
        // One name, one server; no servers can bind to the same name!
        break;
    }
}

void CIntraNameProxy::onReply(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (msg->isStatus())
    {
        if (msg->isError())
        {
            int32_t id;
            std::string reason;
            msg->decodeStatus(id, reason);
            LOG_I("CIntraNameProxy: status is received: msg code: %d, id: %d, reason: %s\n",
                    msg->code(), id, reason.c_str());
        }

        return;
    }

    switch (msg->code())
    {
        case REQ_ALLOC_SERVICE_ADDRESS:
        {
            auto nsp_msg = castToMessage<CNSProxyMsg *>(msg_ref);
            bindAddress(msg, nsp_msg->mReqCtxId, nsp_msg->mReqEpId);
        }
        break;
        default:
        break;
    }
}

void CIntraNameProxy::onOnline(const CFdbOnlineInfo &info)
{
    CBaseNameProxy::onOnline(info);

    CFdbMsgSubscribeList subscribe_list;
    addNotifyItem(subscribe_list, NTF_HOST_INFO);
    if (mNsWatchdogListener)
    {
        addNotifyItem(subscribe_list, NTF_WATCHDOG);
    }
    subscribe(subscribe_list);
    
    queryServiceAddress();
}

void CIntraNameProxy::onOffline(const CFdbOnlineInfo &info)
{
    CBaseNameProxy::onOffline(info);
}

void CIntraNameProxy::registerHostNameReadyNotify(CBaseNotification<CHostNameReady> *notification)
{
    CBaseNotification<CHostNameReady>::Ptr ntf(notification);
    mHostNameNtfCenter.subscribe(ntf);
    if (!hostName().empty())
    {
        CHostNameReady name_ready(hostName());
        mHostNameNtfCenter.notify(name_ready, ntf);
    }
}

void CIntraNameProxy::monitorService(CBaseNotification<FdbMsgAddressList> *notification,
                                     const char *svc_name, FdbInstanceId_t instance_id)
{
    mSvcListNtfCenter.subscribe(notification);

    CEvtHandleTbl evt_tbl;
    evt_tbl.add(instance_id, [this](CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj)
        {
            auto msg = castToMessage<CFdbMessage *>(msg_ref);
            this->notifySvcOnlineMonitor(msg);
        }, 0, svc_name);

    mIntraMonitorSubscriber.registerEventHandle(evt_tbl);
}

void CIntraNameProxy::removeServiceMonitor(CBaseNotification<FdbMsgAddressList> *notification,
                                           const char *svc_name, FdbInstanceId_t instance_id)
{
    CBaseNotification<FdbMsgAddressList>::Ptr ntf(notification);
    mSvcListNtfCenter.unsubscribe(ntf);
    // TODO remove from event handle table
}

class CRegisterWatchdogJob : public CBaseJob
{
public:
    CRegisterWatchdogJob(CIntraNameProxy *ns_proxy, tNsWatchdogListenerFn &watchdog_listener)
        : CBaseJob(JOB_FORCE_RUN)
        , mNsProxy(ns_proxy)
        , mWatchdogListener(watchdog_listener)
    {

    }
protected:
    void run(Ptr &ref)
    {
        mNsProxy->doRegisterNsWatchdogListener(mWatchdogListener);
    }
private:
    CIntraNameProxy *mNsProxy;
    tNsWatchdogListenerFn mWatchdogListener;
};

void CIntraNameProxy::doRegisterNsWatchdogListener(tNsWatchdogListenerFn &watchdog_listener)
{
    mNsWatchdogListener = watchdog_listener;
    if (connected())
    {
        CFdbMsgSubscribeList subscribe_list;
        addNotifyItem(subscribe_list, NTF_WATCHDOG);
        subscribe(subscribe_list);
    }
}

void CIntraNameProxy::registerNsWatchdogListener(tNsWatchdogListenerFn &watchdog_listener)
{
    if (watchdog_listener)
    {
        mContext->sendAsync(new CRegisterWatchdogJob(this, watchdog_listener));
    }
}

void CIntraNameProxy::validateUrl(FdbMsgAddressList &msg_addr_list, CFdbSession *session)
{
    if (!session || msg_addr_list.address_list().pool().empty())
    {
        return;
    }
    
    // make sure 1) UDS and FDB_LOCAL_HOST is preferred
    //           2) FDB_IP_ALL_INTERFACE is replaced with peer address
    //           3) order of interface:
    //              UDS -> FDB_LOCAL_HOST -> identical to peer -> all interface (should be replaced) -> others

    CFdbParcelableArray<FdbMsgAddressItem>::tPool &pool =
                                    msg_addr_list.address_list().vpool();
    CFdbParcelableArray<FdbMsgAddressItem> ipc_candidates;
    CFdbParcelableArray<FdbMsgAddressItem> localhost_candidates;
    CFdbParcelableArray<FdbMsgAddressItem> all_if_candidates;
    CFdbParcelableArray<FdbMsgAddressItem> fallback_candidates;
    for (auto it = pool.begin(); it != pool.end(); ++it)
    {
        if (it->address_type() == FDB_SOCKET_IPC)
        {
            auto item = ipc_candidates.Add();
            *item = *it;
        }
        else if (it->tcp_ipc_address() == FDB_LOCAL_HOST)
        {
            auto item = localhost_candidates.Add();
            *item = *it;
        }
        else if (it->tcp_ipc_address() == FDB_IP_ALL_INTERFACE)
        {
            auto item = all_if_candidates.Add();
            *item = *it;
            item->set_tcp_ipc_address(FDB_LOCAL_HOST);
            CBaseSocketFactory::buildUrl(item->tcp_ipc_url(), FDB_LOCAL_HOST,
                                         it->tcp_port(), it->is_secure());
        }
        else
        {
            auto item = fallback_candidates.Add();
            *item = *it;
        }
    }

    if (!ipc_candidates.empty())
    {
        msg_addr_list.address_list().vpool() = ipc_candidates.vpool();
    }
    else if (!localhost_candidates.empty())
    {
        msg_addr_list.address_list().vpool() = localhost_candidates.vpool();
    }
    else if (!all_if_candidates.empty())
    {
        msg_addr_list.address_list().vpool() = all_if_candidates.vpool();
    }
    else if (!fallback_candidates.empty())
    {
        msg_addr_list.address_list().vpool() = fallback_candidates.vpool();
    }
    else
    {
        LOG_E("CIntraNameProxy: unable to broadcast address for service %s!\n",
              msg_addr_list.service_name().c_str());
    }
}

class CConnectToServerJob : public CMethodJob<CIntraNameProxy>
{
public:
    CConnectToServerJob(CIntraNameProxy *object, bool is_init_response, FdbEndpointId_t ep_id)
        : CMethodJob<CIntraNameProxy>(object, &CIntraNameProxy::callConnectToServer, JOB_FORCE_RUN)
        , mIsInitResponse(is_init_response)
        , mReqEpId(ep_id)
    {}

    FdbMsgAddressList mAddressList;
    bool mIsInitResponse;
    FdbEndpointId_t mReqEpId;
};

void CIntraNameProxy::callConnectToServer(CMethodJob<CIntraNameProxy> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CConnectToServerJob *>(job);
    auto *context = fdb_dynamic_cast_if_available<CFdbBaseContext *>(job->worker());
    doConnectToServer(context, the_job->mReqEpId, the_job->mAddressList, the_job->mIsInitResponse);
}

void CIntraNameProxy::connectToServer(CFdbMessage *msg, FdbContextId_t ctx_id, FdbEndpointId_t ep_id)
{
    auto session = msg->getSession();
    bool is_init_response = msg->isInitialResponse();
    // CIntraNameProxy should run at CFdbContext, not CFdbBaseContext
    auto default_context = fdb_dynamic_cast_if_available<CFdbContext *>(mContext);
    auto &container = default_context->getContexts().getContainer();
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        if (fdbValidFdbId(ctx_id) && (ctx_id != it->first))
        {
            continue;
        }
        auto job = new CConnectToServerJob(this, is_init_response, ep_id);
        CFdbParcelableParser parser(job->mAddressList);
        if (!msg->deserialize(parser))
        {
            delete job;
            return;
        }

        validateUrl(job->mAddressList, session);
        it->second->sendAsyncEndeavor(job);
    }
}

class CBindAddressJob : public CMethodJob<CIntraNameProxy>
{
public:
    CBindAddressJob(CIntraNameProxy *object, bool force_rebind, FdbEndpointId_t ep_id)
        : CMethodJob<CIntraNameProxy>(object, &CIntraNameProxy::callBindAddress, JOB_FORCE_RUN)
        , mForceRebind(force_rebind)
        , mReqEpId(ep_id)
    {}

    FdbMsgAddressList mAddressList;
    bool mForceRebind;
    FdbEndpointId_t mReqEpId;
};

void CIntraNameProxy::callBindAddress(CMethodJob<CIntraNameProxy> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CBindAddressJob *>(job);
    auto *context = fdb_dynamic_cast_if_available<CFdbBaseContext *>(job->worker());
    doBindAddress(context, the_job->mReqEpId, the_job->mAddressList, the_job->mForceRebind);
}

void CIntraNameProxy::bindAddress(CFdbMessage *msg, FdbContextId_t ctx_id, FdbEndpointId_t ep_id)
{
    bool force_rebind = fdbValidFdbId(ctx_id) ? true : false;
    // CIntraNameProxy should run at CFdbContext, not CFdbBaseContext
    auto default_context = fdb_dynamic_cast_if_available<CFdbContext *>(mContext);
    auto &container = default_context->getContexts().getContainer();
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        if (fdbValidFdbId(ctx_id) && (ctx_id != it->first))
        {
            continue;
        }
        auto job = new CBindAddressJob(this, force_rebind, ep_id);
        CFdbParcelableParser parser(job->mAddressList);
        if (!msg->deserialize(parser))
        {
            delete job;
            return;
        }

        it->second->sendAsyncEndeavor(job);
    }
}

class CQueryServiceAddressJob : public CMethodJob<CIntraNameProxy>
{
public:
    CQueryServiceAddressJob(CIntraNameProxy *object)
        : CMethodJob<CIntraNameProxy>(object, &CIntraNameProxy::callQueryServiceAddress, JOB_FORCE_RUN)
    {}
};

void CIntraNameProxy::callQueryServiceAddress(CMethodJob<CIntraNameProxy> *job, CBaseJob::Ptr &ref)
{
    auto *context = fdb_dynamic_cast_if_available<CFdbBaseContext *>(job->worker());
    auto &container = context->getEndpoints().getContainer();
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        auto endpoint = it->second;
        endpoint->requestServiceAddress();
    }
}

void CIntraNameProxy::queryServiceAddress()
{
    // CIntraNameProxy should run at CFdbContext, not CFdbBaseContext
    auto default_context = fdb_dynamic_cast_if_available<CFdbContext *>(mContext);
    auto &container = default_context->getContexts().getContainer();
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        it->second->sendAsyncEndeavor(new CQueryServiceAddressJob(this));
    }
}

void CIntraNameProxy::notifySvcOnlineMonitor(CFdbMessage *msg)
{
    FdbMsgAddressList svc_addr_list;
    CFdbParcelableParser parser(svc_addr_list);
    if (!msg->deserialize(parser))
    {
        return;
    }
    validateUrl(svc_addr_list, msg->getSession());
    mSvcListNtfCenter.notify(svc_addr_list);
}

}
}
