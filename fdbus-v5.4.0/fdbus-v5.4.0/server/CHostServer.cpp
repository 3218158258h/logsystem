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

#include "CHostServer.h"
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CFdbSession.h>
#include <security/CFdbusSecurityConfig.h>
#include <utils/CNsConfig.h>
#include <utils/Log.h>
#include <fdbus/CFdbIfNameServer.h>
#include "CInterHostProxy.h"

namespace ipc {
namespace fdbus {
CHostServer::CHostServer(const char *domain_name,
                         int32_t self_exportable_level,
                         int32_t max_domain_exportable_level,
                         int32_t min_upstream_exportable_level)
    : CBaseServer(domain_name ? domain_name : "Unknown-Domain")
    , mHeartBeatTimer(this)
{
    mMaxDomainExportableLevel = (max_domain_exportable_level < 0) ?
                                FDB_EXPORTABLE_DOMAIN : max_domain_exportable_level;
    mMinUpstreamExportableLevel = (min_upstream_exportable_level < 0) ?
                                  FDB_EXPORTABLE_DOMAIN : min_upstream_exportable_level;
    setExportableLevel((self_exportable_level < 0) ? FDB_EXPORTABLE_SITE : self_exportable_level);
    mHostSecurity.importSecurity();

    mMsgHdl.registerCallback(REQ_REGISTER_LOCAL_HOST, &CHostServer::onRegisterLocalHostReq);
    mMsgHdl.registerCallback(REQ_REGISTER_EXTERNAL_DOMAIN, &CHostServer::onRegisterExternalDomainReq);
    mMsgHdl.registerCallback(REQ_UNREGISTER_LOCAL_HOST, &CHostServer::onUnregisterLocalHostReq);
    mMsgHdl.registerCallback(REQ_QUERY_HOST, &CHostServer::onQueryHostReq);
    mMsgHdl.registerCallback(REQ_HEARTBEAT_OK, &CHostServer::onHeartbeatOk);
    mMsgHdl.registerCallback(REQ_LOCAL_HOST_READY, &CHostServer::onLocalHostReady);
    mMsgHdl.registerCallback(REQ_EXTERNAL_HOST_READY, &CHostServer::onExternalHostReady);
    mMsgHdl.registerCallback(REQ_POST_LOCAL_SVC_ADDRESS, &CHostServer::onPostLocalSvcAddress);
    mMsgHdl.registerCallback(REQ_POST_DOMAIN_SVC_ADDRESS, &CHostServer::onPostDomainSvcAddress);
    mMsgHdl.registerCallback(REQ_HS_QUERY_EXPORTABLE_SERVICE, &CHostServer::onQueryExportableService);

    mSubscribeHdl.registerCallback(NTF_HOST_ONLINE, &CHostServer::onHostOnlineReg);
    mSubscribeHdl.registerCallback(NTF_EXPORT_SVC_ADDRESS_INTERNAL, &CHostServer::onLocalSvcOnlineReg);
    mSubscribeHdl.registerCallback(NTF_EXPORT_SVC_ADDRESS_EXTERNAL, &CHostServer::onExternalSvcOnlineReg);
    mHeartBeatTimer.attach(mContext, false);
}

CHostServer::~CHostServer()
{
}

void CHostServer::online(char **upstream_host_array, int32_t num_upstream_hosts)
{
    std::string hs_url = FDB_URL_SVC;
    hs_url += CNsConfig::getHostServerName();
    bind(hs_url.c_str());
    for (int32_t i = 0; i < num_upstream_hosts; ++i)
    {
        const char *url = upstream_host_array[i];
        if (!url || (url[0] == '\0'))
        {
            continue;
        }
        if (mHostProxyTbl.find(url) != mHostProxyTbl.end())
        {
            continue;
        }
        auto ext_host_proxy = new CInterHostProxy(this, url);
        mHostProxyTbl[url] = ext_host_proxy;
        ext_host_proxy->connectToHostServer();
    }
}

void CHostServer::onSubscribe(CBaseJob::Ptr &msg_ref)
    {
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    const CFdbMsgSubscribeItem *sub_item;
    FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
    {
        mSubscribeHdl.processMessage(this, msg_ref, sub_item, sub_item->msg_code());
    }
    FDB_END_FOREACH_SIGNAL()
}

void CHostServer::onInvoke(CBaseJob::Ptr &msg_ref)
{
    mMsgHdl.processMessage(this, msg_ref);
}

void CHostServer::onOffline(const CFdbOnlineInfo &info)
{
    auto it = mLocalHostTbl.find(info.mSid);
    if (it != mLocalHostTbl.end())
    {
        auto &host = it->second;
        LOG_I("CHostServer: host is dropped: name: %s, ip: %s, ns: %s\n", host.mHostName.c_str(),
                                                                          host.mIpAddress.c_str(),
                                                                          host.mNsUrl.c_str());
        broadcastSingleHost(false, host);
        mLocalHostTbl.erase(it);
        broadcastOnLocalSvcTableChanged();
    }
    else
    {
        auto it = mExternalDomainTbl.find(info.mSid);
        if (it != mExternalDomainTbl.end())
        {
            auto &domain = it->second;
            LOG_I("CHostServer: domain is dropped: name: %s\n", domain.mDomainName.c_str());
            mExternalDomainTbl.erase(it);
            broadcastOnUpstreamSvcTableChanged();
        }
    }

    if ((mLocalHostTbl.size() + mExternalDomainTbl.size()) == 0)
    {
        mHeartBeatTimer.disable();
    }
}

void CHostServer::onRegisterLocalHostReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostRegisterReq host_req;
    CFdbParcelableParser parser(host_req);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    auto ip_addr = host_req.ip_address().c_str();
    auto host_name = host_req.host_name().c_str();
    if (host_req.host_name().empty())
    {
        host_name = ip_addr;
    }

    auto &info = mLocalHostTbl[msg->session()];
    info.mHostName = host_name;
    info.mIpAddress = ip_addr;
    info.mHbCount = 0;
    info.mReady = false;
    info.mAuthorized = true;
    auto cred = mHostSecurity.getCred(host_name);
    if (cred && !cred->empty())
    {
        if (!host_req.has_cred() || host_req.cred().empty() ||
            cred->compare(host_req.cred()))
        {
            info.mAuthorized = false;
        }
    }

    CFdbToken::allocateToken(info.mTokens);

    FdbMsgHostRegisterAck ack;
    populateTokens(info.mTokens, ack);
    ack.set_domain_name(name());
    CFdbParcelableBuilder builder(ack);
    msg->reply(msg_ref, builder);

    if ((mLocalHostTbl.size() + mExternalDomainTbl.size()) == 1)
    {
        mHeartBeatTimer.enable();
    }
}

void CHostServer::onRegisterExternalDomainReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgDomainRegisterReq domain_req;
    CFdbParcelableParser parser(domain_req);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    auto &info = mExternalDomainTbl[msg->session()];
    info.mDomainName = domain_req.domain_name();
    info.mHbCount = 0;
    info.mReady = false;
    info.mAuthorized = true;
    //TODO: check domain cred and deny unauthorized host from being connected

    CFdbToken::allocateToken(info.mTokens);

    FdbMsgHostRegisterAck ack;
    populateTokens(info.mTokens, ack);
    ack.set_domain_name(name());
    CFdbParcelableBuilder builder(ack);
    msg->reply(msg_ref, builder);

    if ((mLocalHostTbl.size() + mExternalDomainTbl.size()) == 1)
    {
        mHeartBeatTimer.enable();
    }
}

void CHostServer::onLocalHostReady(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostAddress host_addr;
    CFdbParcelableParser parser(host_addr);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    auto session = msg->getSession();
    CSvcAddrUtils::verifyUrl(host_addr.address_list(), session);
    auto it = mLocalHostTbl.find(msg->session());
    if (it != mLocalHostTbl.end())
    {
        auto &info = it->second;
        info.mReady = true;

        auto &addrs = host_addr.address_list();
        for (auto it = addrs.vpool().begin(); it != addrs.vpool().end(); ++it)
        {
            info.mAddressTbl.resize(info.mAddressTbl.size() + 1);
            auto &addr_desc = info.mAddressTbl.back();
            it->toSocketAddress(addr_desc);
        }

        CSvcAddrUtils::verifyUrl(host_addr.export_svc_address(), session);
        updateLocalSvcTable(info, host_addr.export_svc_address());

        broadcastSingleHost(true, info);
    }
}

void CHostServer::onExternalHostReady(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgDomainAddress domain_addr;
    CFdbParcelableParser parser(domain_addr);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    //TODO: check host cred and deny unauthorized host from being connected

    auto it = mExternalDomainTbl.find(msg->session());
    if (it != mExternalDomainTbl.end())
    {
        auto &info = it->second;
        info.mReady = true;

        CSvcAddrUtils::verifyUrl(domain_addr.export_svc_address(), msg->getSession());
        updateDomainSvcTable(info, domain_addr.export_svc_address());
    }
}

/*
                  +----+  +----+  +----+
                  | HS |  | HS |  | HS | (upstream)
                  +----+  +----+  +----+
                     |       |       |
           +---------\-------/-------/---------------+   2&3: to local NS (broadcast from HS)
           |          \     /       /                |   1&2: to upstream HS (post from inter host proxy)
           |         +---+---+---+---+               |   1&2&3: to downstream HS (broadcast from HS)
           |         |   |   |   |   | 3-upstream tbl|
           |         +---+---+---+---+               |
           |                                         |
           |   2-dwonstream tbl       1-local tbl    |
           |  +---+---+---+---+  +---+---+---+---+   |
           |  |   |   |   |   |  |   |   |   |   |   |
           |  +---+---+---+---+  +---+---+---+---+   |
           |   /     \      |       \       \     \  |
           +--/------|------|-------|-------|------\-+
             /       |      |       |       |       \
          +----+  +----+  +----+  +----+  +----+  +----+
          | HS |  | HS |  | HS |  | NS |  | NS |  | NS | (downstream)
          +----+  +----+  +----+  +----+  +----+  +----+
 */

// table 1-local tbl
void CHostServer::populateLocalHost(FdbMsgExportableSvcAddress &svc_address_tbl,
                                    int32_t exportable_level)
{
    for (auto host_it = mLocalHostTbl.begin(); host_it != mLocalHostTbl.end(); ++host_it)
    {
        if (host_it->second.mReady)
        {
            CSvcAddrUtils::populateFromHostInfo(host_it->second.mSvcAddrTbl, svc_address_tbl, exportable_level);
        }
    }
}

// table 3-upstream tbl
void CHostServer::populateUpstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl,
                                       int32_t exportable_level)
{
    for (auto host_it = mHostProxyTbl.begin(); host_it != mHostProxyTbl.end(); ++host_it)
    {
        CSvcAddrUtils::populateFromHostInfo(host_it->second->getSvcTable(), svc_address_tbl, exportable_level);
    }
}

// table 2-downstream tbl
void CHostServer::populateDownstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl,
                                         int32_t exportable_level)
{
    for (auto host_it = mExternalDomainTbl.begin(); host_it != mExternalDomainTbl.end(); ++host_it)
    {
        if (host_it->second.mReady)
        {
            CSvcAddrUtils::populateFromHostInfo(host_it->second.mSvcAddrTbl, svc_address_tbl, exportable_level);
        }
    }
}

// table 2 & 3
void CHostServer::buildAddrTblForLocalHost(FdbMsgExportableSvcAddress &svc_address_tbl)
{
    populateUpstreamHost(svc_address_tbl, mMaxDomainExportableLevel);
    populateDownstreamHost(svc_address_tbl, mMaxDomainExportableLevel);
}

// table 1 & 2
void CHostServer::buildAddrTblForUpstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl)
{
    populateDownstreamHost(svc_address_tbl, -mMinUpstreamExportableLevel);
    populateLocalHost(svc_address_tbl, -mMinUpstreamExportableLevel);
}

// table 1 & 2 & 3
void CHostServer::buildAddrTblForDownstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl)
{
    populateUpstreamHost(svc_address_tbl, mMaxDomainExportableLevel);
    populateDownstreamHost(svc_address_tbl, mMaxDomainExportableLevel);
    populateLocalHost(svc_address_tbl, mMaxDomainExportableLevel);
}

void CHostServer::broadcastOnLocalSvcTableChanged()
{
    // table 1 is changed: should 1 - broadcast to downstream host; 2 - post to upstream host
    broadcastToDownstreamHost();
    postToUpstreamHost();
}

void CHostServer::updateLocalSvcTable(CLocalHostInfo &host_info, FdbMsgExportableSvcAddress &svc_address_tbl)
{
    host_info.mSvcAddrTbl.clear();
    CSvcAddrUtils::populateToHostInfo(svc_address_tbl, host_info.mSvcAddrTbl);
    // for local service, domain name is endpoint name of host server.
    for (auto it = host_info.mSvcAddrTbl.begin(); it != host_info.mSvcAddrTbl.end(); ++it)
    {
        it->mDomainName = name();
    }
    broadcastOnLocalSvcTableChanged();
}

void CHostServer::broadcastToLocalHost(CFdbMessage *msg)
{    // broadcast to local hosts
    FdbMsgExportableSvcAddress svc_address_tbl;
    buildAddrTblForLocalHost(svc_address_tbl);
    CFdbParcelableBuilder builder(svc_address_tbl);
    if (msg)
    {
        msg->broadcast(NTF_EXPORT_SVC_ADDRESS_INTERNAL, builder);
    }
    else
    {
        broadcast(NTF_EXPORT_SVC_ADDRESS_INTERNAL, builder);
    }
}

void CHostServer::broadcastToDownstreamHost(CFdbMessage *msg)
{    // broadcast to local hosts
    FdbMsgExportableSvcAddress svc_address_tbl;
    buildAddrTblForDownstreamHost(svc_address_tbl);
    CFdbParcelableBuilder builder(svc_address_tbl);
    if (msg)
    {
        msg->broadcast(NTF_EXPORT_SVC_ADDRESS_EXTERNAL, builder);
    }
    else
    {
        broadcast(NTF_EXPORT_SVC_ADDRESS_EXTERNAL, builder);
    }
}

void CHostServer::postToUpstreamHost()
{
    FdbMsgExportableSvcAddress svc_address_tbl;
    buildAddrTblForUpstreamHost(svc_address_tbl);
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {   // synchronize with upstream host servers
        it->second->postSvcTable(svc_address_tbl);
    }
}

void CHostServer::onPostLocalSvcAddress(CBaseJob::Ptr &msg_ref)
{
    FdbMsgExportableSvcAddress svc_address_tbl;
    CFdbParcelableParser parser(svc_address_tbl);
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }
    
    auto it = mLocalHostTbl.find(msg->session());
    if (it != mLocalHostTbl.end())
    {
        CSvcAddrUtils::verifyUrl(svc_address_tbl, msg->getSession());
        updateLocalSvcTable(it->second, svc_address_tbl);
    }
}

void CHostServer::broadcastOnDomainSvcTableChanged()
{
    // table-2 is changed: should notify local hosts and downstream hosts meanwhile sync
    // upstream hosts
    broadcastToLocalHost();
    broadcastToDownstreamHost();
    postToUpstreamHost();
}

void CHostServer::updateDomainSvcTable(CExternalDomainInfo &domain_info,
                                       FdbMsgExportableSvcAddress &svc_address_tbl)
{
    domain_info.mSvcAddrTbl.clear();
    CSvcAddrUtils::populateToHostInfo(svc_address_tbl, domain_info.mSvcAddrTbl);
    broadcastOnDomainSvcTableChanged();
}

void CHostServer::onPostDomainSvcAddress(CBaseJob::Ptr &msg_ref)
{
    FdbMsgExportableSvcAddress svc_address_tbl;
    CFdbParcelableParser parser(svc_address_tbl);
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    CSvcAddrUtils::verifyUrl(svc_address_tbl, msg->getSession());

    auto it = mExternalDomainTbl.find(msg->session());
    if (it != mExternalDomainTbl.end())
    {
        updateDomainSvcTable(it->second, svc_address_tbl);
    }
}

void CHostServer::onQueryExportableService(CBaseJob::Ptr &msg_ref)
{
    FdbMsgAllExportableService exp_service;
    populateLocalHost(exp_service.local_table(), FDB_EXPORTABLE_ANY);
    populateUpstreamHost(exp_service.upstream_table(), FDB_EXPORTABLE_ANY);
    populateDownstreamHost(exp_service.downstream_table(), FDB_EXPORTABLE_ANY);
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    CFdbParcelableBuilder builder(exp_service);
    msg->reply(msg_ref, builder);
}

void CHostServer::broadcastOnUpstreamSvcTableChanged()
{
    // table-3 is changed: should 1-broadcast to local hosts; 2-broadcast to downstream hosts
    broadcastToLocalHost();
    broadcastToDownstreamHost();
}
void CHostServer::updateUpstreamSvcTable(tSvcAddrDescTbl &svc_desc_tbl,
                                         FdbMsgExportableSvcAddress &svc_address_tbl)
{
    svc_desc_tbl.clear();
    CSvcAddrUtils::populateToHostInfo(svc_address_tbl, svc_desc_tbl);
    for (auto it = mLocalHostTbl.begin(); it != mLocalHostTbl.end(); ++it)
    {
        CSvcAddrUtils::removeDuplicateService(svc_desc_tbl, it->second.mSvcAddrTbl);
    }
    broadcastOnUpstreamSvcTableChanged();
}

// give token from 'this_host' to 'that_host' so that 'that_host' can connect with 'this_host'
void CHostServer::addToken(const CLocalHostInfo &this_host,
                          const CLocalHostInfo &that_host,
                          FdbMsgHostAddress &host_addr)
{
    int32_t security_level;
    if (that_host.mAuthorized)
    {
         security_level =  mHostSecurity.getSecurityLevel(this_host.mHostName.c_str(),
                                                          that_host.mHostName.c_str());
    }
    else
    {
        security_level = FDB_SECURITY_LEVEL_NONE;
    }

    const char *token = 0;
    if ((security_level >= 0) && (security_level < (int32_t)this_host.mTokens.size()))
    {
        token = this_host.mTokens[security_level].c_str();
    }
    host_addr.token_list().clear_tokens();
    if (token)
    {
        host_addr.token_list().add_tokens(token);
    }
}

FdbMsgHostAddress *CHostServer::populateHostAddressList(CFdbSession *session,
                                    FdbMsgHostAddressList &addr_list, CLocalHostInfo &info)
{
    auto addr = addr_list.add_address_list();
    addr->set_host_name(info.mHostName);
    // TODO: session->hostIp(interface_ip) can also be used!!!
    addr->set_ip_address(info.mIpAddress);
    if (info.mIpAddress.empty() || (info.mIpAddress == FDB_IP_ALL_INTERFACE))
    {
        if (session)
        {
            std::string interface_ip;
            session->hostIp(interface_ip);
            addr->set_ip_address(interface_ip);
        }
    }

    for (auto it = info.mAddressTbl.begin(); it != info.mAddressTbl.end(); ++it)
    {
        auto host_addr = addr->add_address_list();
        host_addr->fromSocketAddress(*it);
        host_addr->set_udp_port(FDB_INET_PORT_INVALID);
    }
    return addr;
}

void CHostServer::broadcastSingleHost(bool online, CLocalHostInfo &info)
{
    if (online)
    {
        tSubscribedSessionSets sessions;
        getSubscribeTable(NTF_HOST_ONLINE, 0, sessions);
        for (auto session_it = sessions.begin(); session_it != sessions.end(); ++session_it)
        {
            CFdbSession *session = *session_it;
            auto info_it = mLocalHostTbl.find(session->sid());
            CLocalHostInfo *that_info = 0;
            if (info_it != mLocalHostTbl.end())
            {
                that_info = &info_it->second;
                if ((that_info == &info) || !that_info->mReady)
                {
                    continue;
                }
            }

            FdbMsgHostAddressList addr_list;
            auto addr = populateHostAddressList(session, addr_list, info);
            if (that_info)
            {
                addToken(info, *that_info, *addr);
            }
            buildAddrTblForLocalHost(addr_list.export_svc_address());
            if (!addr_list.address_list().empty())
            {
                CFdbParcelableBuilder builder(addr_list);
                broadcast(session->sid(), NTF_HOST_ONLINE, builder);
            }
        }
    }
    else
    {
        FdbMsgHostAddressList addr_list;
        populateHostAddressList(0, addr_list, info);
        if (!addr_list.address_list().empty())
        {
            CFdbParcelableBuilder builder(addr_list);
            broadcast(NTF_HOST_ONLINE, builder);
        }
    }
}

void CHostServer::onUnregisterLocalHostReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostAddress host_addr;
    CFdbParcelableParser parser(host_addr);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }
    auto ip_addr = host_addr.ip_address().c_str();

    for (auto it = mLocalHostTbl.begin(); it != mLocalHostTbl.end(); ++it)
    {
        if (it->second.mIpAddress == ip_addr)
        {
            CLocalHostInfo &info = it->second;
            LOG_I("CHostServer: host is unregistered: name: %s, ip: %s, ns: %s\n",
                    info.mHostName.c_str(), info.mIpAddress.c_str(), info.mNsUrl.c_str());
            broadcastSingleHost(false, info);
            mLocalHostTbl.erase(it);
            return;
        }
    }
    msg->status(msg_ref, FDB_ST_NON_EXIST);
}

void CHostServer::onQueryHostReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    msg->status(msg_ref, FDB_ST_NOT_IMPLEMENTED, "onQueryHostReq() is not implemented!");
}

void CHostServer::onHeartbeatOk(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    auto it = mLocalHostTbl.find(msg->session());

    if (it != mLocalHostTbl.end())
    {
        auto &info = it->second;
        info.mHbCount = 0;
    }
    else
    {
        auto it = mExternalDomainTbl.find(msg->session());
        if (it != mExternalDomainTbl.end())
        {
            auto &domain = it->second;
            domain.mHbCount = 0;
        }
    }
}

void CHostServer::onHostOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostAddressList addr_list;
    auto session = msg->getSession();
    auto info_it = mLocalHostTbl.find(session->sid());
    CLocalHostInfo *that_info = 0;
    if (info_it != mLocalHostTbl.end())
    {
        that_info = &info_it->second;
    }
    for (auto it = mLocalHostTbl.begin(); it != mLocalHostTbl.end(); ++it)
    {
        auto &this_info = it->second;
        if ((&this_info == that_info) || !this_info.mReady)
        {
            continue;
        }
        auto addr = populateHostAddressList(session, addr_list, this_info);
        if (that_info)
        {
            addToken(this_info, *that_info, *addr);
        }
    }
    buildAddrTblForLocalHost(addr_list.export_svc_address());

    if (!addr_list.address_list().empty())
    {
        CFdbParcelableBuilder builder(addr_list);
        msg->broadcast(NTF_HOST_ONLINE, builder);
    }
}

void CHostServer::onLocalSvcOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    broadcastToLocalHost(msg);
}

void CHostServer::onExternalSvcOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    broadcastToDownstreamHost(msg);
}

void CHostServer::broadcastHeartBeat(CMethodLoopTimer<CHostServer> *timer)
{
    for (auto it = mLocalHostTbl.begin(); it != mLocalHostTbl.end();)
    {
        auto the_it = it++;
        auto &info = the_it->second;
        if (++info.mHbCount >= CNsConfig::getHeartBeatRetryNr())
        {
            // will trigger offline callback which do everything for me.
            LOG_E("Host %s is kicked out due to HB!\n", info.mHostName.c_str());
            kickOut(the_it->first);
        }
    }

    for (auto it = mExternalDomainTbl.begin(); it != mExternalDomainTbl.end();)
    {
        auto the_it = it++;
        auto &info = the_it->second;
        if (++info.mHbCount >= CNsConfig::getHeartBeatRetryNr())
        {
            // will trigger offline callback which do everything for me.
            LOG_E("Domain %s is kicked out due to HB!\n", info.mDomainName.c_str());
            kickOut(the_it->first);
        }
    }

    broadcast(NTF_HEART_BEAT);
}

void CHostServer::populateTokens(const CFdbToken::tTokenList &tokens,
                                 FdbMsgHostRegisterAck &list)
{
    list.token_list().clear_tokens();
    for (CFdbToken::tTokenList::const_iterator it = tokens.begin(); it != tokens.end(); ++it)
    {
        list.token_list().add_tokens(*it);
    }
}
}
}
