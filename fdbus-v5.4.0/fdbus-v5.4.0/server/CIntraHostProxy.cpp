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

#include <vector>
#include "CIntraHostProxy.h"
#include "CInterNameProxy.h"
#include <fdbus/CFdbContext.h>
#include "CNameServer.h"
#include <fdbus/CFdbIfNameServer.h>
#include <utils/Log.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CFdbSession.h>
#include <algorithm>

namespace ipc {
namespace fdbus {
CIntraHostProxy::CIntraHostProxy(CNameServer *ns)
    : CNameProxyContainer(ns)
{
}

void CIntraHostProxy::isolate(tNameProxyTbl::iterator &it)
{
    auto ns_proxy = it->second;
    ns_proxy->doDisconnect();
    LOG_E("CIntraHostProxy: Disconnected to NS of host %s.\n", ns_proxy->getHostIp().c_str());
    // eventually call deleteNameProxy()
    delete ns_proxy;
}

void CIntraHostProxy::isolate()
{
    for (auto it = mNameProxyTbl.begin(); it != mNameProxyTbl.end();)
    {
        auto the_it = it;
        ++it;
        isolate(the_it);
    }
}

CIntraHostProxy::~CIntraHostProxy()
{
    isolate();
    doDisconnect();
}

void CIntraHostProxy::onOnline(const CFdbOnlineInfo &info)
{
    CBaseHostProxy::onOnline(info);
    if (mSelfIp.empty())
    {
        auto session = getSession(info.mSid);
        session->hostIp(mSelfIp);
    }

    hostOnline();

    LOG_I("CIntraHostProxy: connected to host server: %s\n.", mHostUrl.c_str());
}

void CIntraHostProxy::onOffline(const CFdbOnlineInfo &info)
{
    mNameServer->onHostOnline(false, this);
    
    FdbMsgHostAddressList host_list;
    mNameServer->getHostTbl(host_list, getSession(info.mSid));
    CFdbParcelableBuilder builder(host_list);
    mNameServer->broadcast(NTF_HOST_ONLINE_LOCAL, builder);

    LOG_E("CIntraHostProxy: Name Proxy: Fail to connect address: %s. Reconnecting...\n",
            mHostUrl.c_str());
    CBaseHostProxy::onOffline(info);
}

void CIntraHostProxy::onReply(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (msg->isStatus())
    {
        int32_t id;
        std::string reason;
        msg->decodeStatus(id, reason);
        LOG_I("CIntraHostProxy: onReply(): status is received: msg code: %d, id: %d, reason: %s\n",
              msg->code(), id, reason.c_str());
        if (msg->code() == REQ_HS_QUERY_EXPORTABLE_SERVICE)
        {
            mNameServer->finalizeExportableServiceQuery(0, msg);
        }
        return;
    }

    switch (msg->code())
    {
        case REQ_REGISTER_LOCAL_HOST:
        {
            FdbMsgHostRegisterAck ack;
            CFdbParcelableParser parser(ack);
            if (!msg->deserialize(parser))
            {
                return;
            }
            if (ack.has_token_list() && mNameServer->importTokens(ack.token_list().tokens()))
            {
                mNameServer->updateSecurityLevel();
                LOG_I("CIntraHostProxy: tokens of name server is updated.\n");
            }
            mDomainName = ack.domain_name();

            // onHostOnline() should be called before sending REQ_LOCAL_HOST_READY
            // because it will bind TCP addresses for name server, thereby
            // mNameServer->populateNsAddrList() can get a list of address of name server
            // However, mNameServer->populateExportAddrList() may not be able to
            // retrieve exported service address since the TCP address is not bound.
            // But once the addresses is retrieved, they will be sent to host server
            // in postSvcTable() from CNameServer:: onRegisterServiceReq()
            mNameServer->onHostOnline(true, this);

            const char *host_ip;
            if (mSelfIp.empty() || (mSelfIp == FDB_LOCAL_HOST))
            {
                host_ip = FDB_IP_ALL_INTERFACE;
            }
            else
            {
                host_ip = mSelfIp.c_str();
            }
            FdbMsgHostAddress host_addr;
            host_addr.set_ip_address(host_ip);
            host_addr.set_host_name(mNameServer->hostName());
            // The interface connecting to host server takes precedence
            mNameServer->populateNsAddrList(host_addr);
            mNameServer->populateExportAddrList(host_addr.export_svc_address());

            CFdbParcelableBuilder builder(host_addr);
            send(REQ_LOCAL_HOST_READY, builder);

            // The subscription of NTF_HOST_ONLINE shall be placed after REQ_LOCAL_HOST_READY
            // otherwise mHostTbl.find(session->sid()) in CHostServer::onHostOnlineReg()
            // fails
            {
                CFdbMsgSubscribeList subscribe_list;
                addNotifyItem(subscribe_list, NTF_HOST_ONLINE);
                addNotifyItem(subscribe_list, NTF_EXPORT_SVC_ADDRESS_INTERNAL);
                subscribe(subscribe_list, 0);
            }
        }
        break;
        case REQ_HS_QUERY_EXPORTABLE_SERVICE:
        {
            FdbMsgAllExportableService svc_tbl;
            CFdbParcelableParser parser(svc_tbl);
            if (msg->deserialize(parser))
            {
                mNameServer->finalizeExportableServiceQuery(&svc_tbl, msg);
            }
            else
            {
                mNameServer->finalizeExportableServiceQuery(0, msg);
            }
        }
        break;
        default:
        break;
    }
}

void CIntraHostProxy::broadcastOneExtService(CFdbSvcAddress &exported_addr, bool remove,
                                             SubscribeType subscribe_type, CFdbMessage *msg)
{
    if (!connected() || mNameServer->isLocalService(exported_addr))
    {   // if the service is a local one (local service exporting external address),
        // do not broadcast internally
        return;
    }
    FdbMsgAddressList addr_list;
    CSvcAddrUtils::populateFromHostInfo(exported_addr, addr_list,
                                        mNameServer->maxDomainExportableLevel(), remove);
    CFdbToken::tTokenList tokens;
    addr_list.dumpTokens(tokens);
    addr_list.token_list().clear_tokens();
    // TODO if more than one addresses are exported by service, it is not possible
    // to broadcast all of them to clients. Usually a client only connects to
    // the service via single address (or two: one normal and one secure). Thereby
    // we should only keep one in the address list.
    if (msg)
    {
        if (subscribe_type == INTRA_NORMAL)
        {
            mNameServer->broadcastSvcAddrLocal(tokens, addr_list, msg);
        }
        else
        {
            CFdbParcelableBuilder builder(addr_list);
            msg->broadcast(exported_addr.mInstanceId, builder, exported_addr.mSvcName.c_str());
        }
    }
    else
    {
        if (subscribe_type == INTRA_NORMAL)
        {
            mNameServer->broadcastSvcAddrLocal(tokens, addr_list);
        }
        else
        {
            CFdbParcelableBuilder builder(addr_list);
            mNameServer->broadcastService(exported_addr.mInstanceId, builder,
                                          exported_addr.mSvcName.c_str(), subscribe_type);
        }
    }
}

void CIntraHostProxy::broadcastExternalService(tSvcAddrDescTbl &svc_addr_tbl,
                                               bool remove,
                                               SubscribeType subscribe_type,
                                               FdbInstanceId_t instance_id,
                                               const char *svc_name,
                                               CFdbMessage *msg)
{
    if (svc_name && (svc_name[0] != '\0'))
    {
        bool is_group_of_instance = fdbIsGroup(instance_id);
        bool broadcast_all_instance = fdbEventInGroup(instance_id, FDB_EVENT_ALL_GROUPS);
        for (auto it = svc_addr_tbl.begin(); it != svc_addr_tbl.end(); ++it)
        {
            bool shall_broadcast = false;
            if (it->mSvcName == svc_name)
            {
                if (is_group_of_instance)
                {
                    if (broadcast_all_instance || fdbSameGroup(instance_id, it->mInstanceId))
                    {
                        shall_broadcast = true;
                    }
                }
                else if (it->mInstanceId == instance_id)
                {
                    shall_broadcast = true;
                }
                if (shall_broadcast)
                {
                    broadcastOneExtService(*it, remove, subscribe_type, msg);
                }
            }
        }
    }
    else
    {
        for (auto it = svc_addr_tbl.begin(); it != svc_addr_tbl.end(); ++it)
        {
            broadcastOneExtService(*it, remove, subscribe_type, msg);
        }
    }
}

void CIntraHostProxy::broadcastExternalService(bool remove,
                                               SubscribeType subscribe_type,
                                               FdbInstanceId_t instance_id,
                                               const char *svc_name,
                                               CFdbMessage *msg)
{
    broadcastExternalService(mSvcAddrTbl, remove, subscribe_type, instance_id, svc_name, msg);
}

void CIntraHostProxy::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (msg->isStatus())
    {
        int32_t id;
        std::string reason;
        msg->decodeStatus(id, reason);
        LOG_I("CIntraHostProxy: onBroadcast(): status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());
        return;
    }

    switch (msg->code())
    {
        case NTF_HOST_ONLINE:
        {
            onHostOnlineNotify(msg);
            break;
        }
        case NTF_EXPORT_SVC_ADDRESS_INTERNAL:
        {
            FdbMsgExportableSvcAddress svc_address_tbl;
            CFdbParcelableParser parser(svc_address_tbl);
            if (!msg->deserialize(parser))
            {
                return;
            }
            tSvcAddrDescTbl added_svc_tbl;
            tSvcAddrDescTbl removed_svc_tbl;
            CSvcAddrUtils::compareSvcTable(svc_address_tbl, mSvcAddrTbl, added_svc_tbl, removed_svc_tbl);

            LOG_I("CIntraHostProxy: public service table is updated. New service:\n");
            CSvcAddrUtils::printExternalSvcTbl(added_svc_tbl, "    ");
            LOG_I("Removed service:\n");
            CSvcAddrUtils::printExternalSvcTbl(removed_svc_tbl, "    ");

            broadcastExternalService(added_svc_tbl, false, INTRA_NORMAL);
            broadcastExternalService(removed_svc_tbl, true, INTRA_NORMAL);
            broadcastExternalService(added_svc_tbl, false, INTRA_MONITOR);
            broadcastExternalService(removed_svc_tbl, true, INTRA_MONITOR);

            mSvcAddrTbl.clear();
            CSvcAddrUtils::populateToHostInfo(svc_address_tbl, mSvcAddrTbl);
            break;
        }
        default:
            CBaseHostProxy::onBroadcast(msg_ref);
        break;
    }
}

bool CIntraHostProxy::getNsAddress(FdbMsgHostAddress &host_addr, CFdbSession *session,
                              std::string &ns_ip, std::string &ns_url)
{
    const char *most_best_secure_url = 0;
    const char *most_best_secure_ip = 0;

    const char *most_best_insecure_url = 0;
    const char *most_best_insecure_ip = 0;

    const char *second_best_secure_url = 0;
    const char *second_best_secure_ip = 0;

    const char *second_best_insecure_url = 0;
    const char *second_best_insecure_ip = 0;

    const char *fallback_secure_url = 0;
    const char *fallback_secure_ip = 0;

    const char *fallback_insecure_url = 0;
    const char *fallback_insecure_ip = 0;

    std::string all_interface_secure_url;
    const char *all_interface_secure_ip = 0;

    std::string all_interface_insecure_url;
    const char *all_interface_insecure_ip = 0;

    std::string peer_ip;

    session->peerIp(peer_ip);
    auto &addrs = host_addr.address_list();
    for (auto it = addrs.vpool().begin(); it != addrs.vpool().end(); ++it)
    {
        if (it->tcp_ipc_address() == FDB_LOCAL_HOST)
        {
            continue;
        }
        if ((it->tcp_ipc_address() == FDB_IP_ALL_INTERFACE) &&
            !peer_ip.empty() && (peer_ip != FDB_LOCAL_HOST))
        {
            if (it->is_secure())
            {
                if (all_interface_secure_url.empty())
                {
                    all_interface_secure_ip = peer_ip.c_str();
                    CBaseSocketFactory::buildUrl(all_interface_secure_url, all_interface_secure_ip,
                                                 it->tcp_port(), true);
                }
            }
            else
            {
                if (all_interface_insecure_url.empty())
                {
                    all_interface_insecure_ip = peer_ip.c_str();
                    CBaseSocketFactory::buildUrl(all_interface_insecure_url, all_interface_insecure_ip,
                                                 it->tcp_port(), false);
                }
            }
        }
        else if (mNameServer->sameSubnet(it->tcp_ipc_address().c_str()))
        {
            if (it->is_secure())
            {
                if (!most_best_secure_url)
                {
                    most_best_secure_ip = it->tcp_ipc_address().c_str();
                    most_best_secure_url = it->tcp_ipc_url().c_str();
                }
            }
            else
            {
                if (!most_best_insecure_url)
                {
                    most_best_insecure_ip = it->tcp_ipc_address().c_str();
                    most_best_insecure_url = it->tcp_ipc_url().c_str();
                }
            }
        }
        else if (it->tcp_ipc_address() == host_addr.ip_address())
        {
            if (it->is_secure())
            {
                if (!second_best_secure_url)
                {
                    second_best_secure_ip = it->tcp_ipc_address().c_str();
                    second_best_secure_url = it->tcp_ipc_url().c_str();
                }
            }
            else
            {
                if (!second_best_insecure_url)
                {
                    second_best_insecure_ip = it->tcp_ipc_address().c_str();
                    second_best_insecure_url = it->tcp_ipc_url().c_str();
                }
            }
        }
        else
        {
            if (it->is_secure())
            {
                if (!fallback_secure_url)
                {
                    fallback_secure_ip = it->tcp_ipc_address().c_str();
                    fallback_secure_url = it->tcp_ipc_url().c_str();
                }
            }
            else
            {
                if (!fallback_insecure_url)
                {
                    fallback_insecure_ip = it->tcp_ipc_address().c_str();
                    fallback_insecure_url = it->tcp_ipc_url().c_str();
                }
            }
        }
    }

    const char *select_secure_ip = 0;
    const char *select_secure_url = 0;
    if (most_best_secure_url)
    {
        select_secure_ip = most_best_secure_ip;
        select_secure_url = most_best_secure_url;
    }
    else if (second_best_secure_url)
    {
        select_secure_ip = second_best_secure_ip;
        select_secure_url = second_best_secure_url;
    }
    else if (!all_interface_secure_url.empty())
    {
        select_secure_ip = all_interface_secure_ip;
        select_secure_url = all_interface_secure_url.c_str();
    }
    else
    {
        select_secure_ip = fallback_secure_ip;
        select_secure_url = fallback_secure_url;
    }

    const char *select_insecure_ip = 0;
    const char *select_insecure_url = 0;
    if (most_best_insecure_url)
    {
        select_insecure_ip = most_best_insecure_ip;
        select_insecure_url = most_best_insecure_url;
    }
    else if (second_best_insecure_url)
    {
        select_insecure_ip = second_best_insecure_ip;
        select_insecure_url = second_best_insecure_url;
    }
    else if (!all_interface_insecure_url.empty())
    {
        select_insecure_ip = all_interface_insecure_ip;
        select_insecure_url = all_interface_insecure_url.c_str();
    }
    else
    {
        select_insecure_ip = fallback_insecure_ip;
        select_insecure_url = fallback_insecure_url;
    }

    const char *selected_ip = 0;
    const char *selected_url = 0;
    // connect with other name server with secure channel if enabled
    if (mNameServer->TCPSecureEnabled())
    {
        selected_ip = select_secure_ip;
        selected_url = select_secure_url;
        if (!selected_url)
        {
            selected_ip = select_insecure_ip;
            selected_url = select_insecure_url;
        }
    }
    else
    {
        selected_ip = select_insecure_ip;
        selected_url = select_insecure_url;
    }

    if (selected_ip && selected_url)
    {
        ns_ip = selected_ip;
        ns_url = selected_url;
        return true;
    }

    return false;
}

void CIntraHostProxy::onHostOnlineNotify(CFdbMessage *msg)
{
    FdbMsgHostAddressList host_list;
    auto session = msg->getSession();
    if (!session)
    {
        return; //never happen!!!
    }
    CFdbParcelableParser parser(host_list);
    if (!msg->deserialize(parser))
    {
        return;
    }

    std::string host_ip;
    session->hostIp(host_ip);

    CFdbParcelableBuilder builder(host_list);
    mNameServer->broadcast(NTF_HOST_ONLINE_LOCAL, builder);
    auto &addr_list = host_list.address_list();
    std::vector<std::string> online_host_list;
    for (auto it = addr_list.vpool().begin(); it != addr_list.vpool().end(); ++it)
    {
        auto &addr = *it;

        std::string ns_ip;
        std::string ns_url;
        if (!getNsAddress(addr, session, ns_ip, ns_url))
        {
            LOG_E("CIntraHostProxy: unable to obtain url of name server for host %s!\n", addr.host_name().c_str());
            continue;
        }

        if ((ns_ip == host_ip) || (addr.host_name() == mNameServer->hostName()) ||
                (ns_ip == FDB_LOCAL_HOST) || (ns_ip == FDB_IP_ALL_INTERFACE))
        {   // don't connect to self.
            continue;
        }

        auto np_it = mNameProxyTbl.find(ns_ip);
        if (addr.address_list().pool().empty())
        {
            if (np_it != mNameProxyTbl.end())
            {
                mNameServer->notifyRemoteNameServerDrop(np_it->second->getHostName().c_str());
                delete np_it->second;
            }
        }
        else
        {
            online_host_list.push_back(ns_ip);
            if (np_it == mNameProxyTbl.end())
            {
                auto proxy = new CInterNameProxy(this,
                                                 ns_ip.c_str(),
                                                 ns_url.c_str(),
                                                 addr.host_name().c_str());
                if (mNameServer->TCPSecureEnabled())
                {   // TODO: enable secure channel of proxy here!
                }
                proxy->enableReconnect(true);
                proxy->autoRemove(true);
                // The token will be sent to peer name server to determine priority of this
                // host. Token between CInterNameProxy and CNameServer is not used to check
                // priority of method call from CInterNameProxy to CNameServer. Actually it
                // is used to check priority of the host. For details please refers to 
                // CNameServer::populateTokensRemote()
                if (addr.has_token_list())
                {
                    proxy->importTokens(addr.token_list().tokens());
                }
                addNameProxy(proxy, ns_ip.c_str());
                proxy->connectToNameServer();
            }
        }
    }

    if (msg->isInitialResponse() && !online_host_list.empty())
    {
        for (auto np_it = mNameProxyTbl.begin(); np_it != mNameProxyTbl.end();)
        {
            auto the_np_it = np_it;
            ++np_it;
            auto proxy = the_np_it->second;
            if (std::find(online_host_list.begin(), online_host_list.end(), proxy->getHostIp()) ==
                    online_host_list.end())
            {
                isolate(the_np_it);
            }
        }
    }
}

void CIntraHostProxy::hostOnline(FdbMsgCode_t code)
{
    const char *host_ip;
    if (mSelfIp.empty() || (mSelfIp == FDB_LOCAL_HOST))
    {
        host_ip = FDB_IP_ALL_INTERFACE;
    }
    else
    {
        host_ip = mSelfIp.c_str();
    }

    FdbMsgHostRegisterReq host_req;
    host_req.set_ip_address(host_ip);
    host_req.set_host_name(mNameServer->hostName());
    // TODO: set cred!!!
    CFdbParcelableBuilder builder(host_req);
    invoke(code, builder);
}

void CIntraHostProxy::hostOnline()
{
    hostOnline(REQ_REGISTER_LOCAL_HOST);
}

void CIntraHostProxy::hostOffline()
{
    hostOnline(REQ_UNREGISTER_LOCAL_HOST);
}

void CIntraHostProxy::postSvcTable(FdbMsgExportableSvcAddress &svc_address_tbl)
{
    CFdbParcelableBuilder builder(svc_address_tbl);
    if (connected())
    {
        send(REQ_POST_LOCAL_SVC_ADDRESS, builder);
    }
}

void CIntraHostProxy::dumpExportedSvcInfo(FdbMsgExportableSvcAddress &svc_info_tbl)
{
    CSvcAddrUtils::populateFromHostInfo(mSvcAddrTbl, svc_info_tbl, FDB_EXPORTABLE_ANY);
}

bool CIntraHostProxy::sendQueryExportableServiceReq(CFdbMessage *msg)
{
    return connected() ? invoke(msg, 0, 0, FDB_QUERY_MAX_TIMEOUT) : false;
}
}
}
