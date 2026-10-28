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
#include <stdio.h>
#include <vector>
#include "CNameServer.h"
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CFdbSession.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CFdbIfNameServer.h>
#include <security/CFdbusSecurityConfig.h>
#include "CIntraHostProxy.h"
#include <utils/CNsConfig.h>
#include <utils/Log.h>
#include <fdbus/CFdbWatchdog.h>
#include "CInterNameProxy.h"
#include <algorithm>

namespace ipc {
namespace fdbus {
#define FDB_HOST_NAME_MAX 1024

class CQueryExportedServiceMsg : public CFdbMessage
{
public:
    CQueryExportedServiceMsg(tPendingServiceReqTbl *pending_tbl,
                             FdbMsgAllExportableService *svc_tbl,
                             CBaseJob::Ptr &req,
                             const std::string &host_server_ip,
                             CNameServer *name_server)
        : CFdbMessage(REQ_HS_QUERY_EXPORTABLE_SERVICE)
        , mPendingReqTbl(pending_tbl)
        , mSvcTbl(svc_tbl)
        , mReq(req)
        , mHostServerIp(host_server_ip)
        , mNameServer(name_server)
    {}
    ~CQueryExportedServiceMsg()
    {}
    
    tPendingServiceReqTbl *mPendingReqTbl;
    FdbMsgAllExportableService *mSvcTbl;
    CBaseJob::Ptr mReq;
    std::string mHostServerIp;
    CNameServer *mNameServer;
protected:
    void onAsyncError(Ptr &ref, FdbMsgStatusCode code, const char *reason)
    {
        mNameServer->finalizeExportableServiceQuery(0, this);
    }
};

CNameServer::ConnectAddrPublisher::ConnectAddrPublisher(CNameServer *ns, SubscribeType type,
                                                        const char *name)
    : CFdbBaseObject(name)
    , mNameServer(ns)
    , mType(type)
{
}

void CNameServer::ConnectAddrPublisher::bindToNs()
{
    // type is regarded as object ID
    bind(mNameServer, mType);
}

void CNameServer::ConnectAddrPublisher::onSubscribe(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    const CFdbMsgSubscribeItem *sub_item;
    /* iterate all message id subscribed */
    FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
    {
        FdbMsgCode_t instance_id = sub_item->msg_code();
        const char *svc_name = "";
        if (sub_item->has_topic())
        {
            svc_name = sub_item->topic().c_str();
        }
        mNameServer->onServiceOnlineReg(svc_name, instance_id, msg_ref, mType);
    }
    FDB_END_FOREACH_SIGNAL()
}

CNameServer::BindAddrPublisher::BindAddrPublisher(CNameServer *ns, const char *name)
    : CFdbBaseObject(name)
    , mNameServer(ns)
{
}

void CNameServer::BindAddrPublisher::bindToNs()
{
    // type is regarded as object ID
    bind(mNameServer, MORE_ADDRESS);
}

CNameServer::CNameServer(const char *host_name,
                         int32_t self_exportable_level,
                         int32_t max_domain_exportable_level,
                         int32_t min_upstream_exportable_level)
    : CBaseServer()
    , mStaticNameProxy(this)
    , mIntraNormalPublisher(this, INTRA_NORMAL, "Intra SVC publisher")
    , mIntraMonitorPublisher(this, INTRA_MONITOR, "Intra SVC monitor")
    , mInterNormalPublisher(this, INTER_NORMAL, "Inter SVC publisher")
    , mInterMonitorPublisher(this, INTER_MONITOR, "Inter SVC monitor")
    , mBindAddrPublisher(this, "Bind address publisher")
    , mInstanceIdAllocator(FDB_AUTO_INSTANCE_BEGIN)
{
    mMaxDomainExportableLevel = (max_domain_exportable_level < 0) ?
                                FDB_EXPORTABLE_DOMAIN : max_domain_exportable_level;
    mMinUpstreamExportableLevel = (min_upstream_exportable_level < 0) ?
                                  FDB_EXPORTABLE_DOMAIN : min_upstream_exportable_level;
    setExportableLevel((self_exportable_level < 0) ? FDB_EXPORTABLE_DOMAIN : self_exportable_level);
    setNsName(CNsConfig::getNameServerName());
    mServerSecruity.importSecurity();
    role(FDB_OBJECT_ROLE_NS_SERVER);

    if (host_name)
    {
        mHostName = host_name;
    }
    else
    {
        char buffer[FDB_HOST_NAME_MAX];
        sysdep_gethostname(buffer, FDB_HOST_NAME_MAX);
        buffer[FDB_HOST_NAME_MAX - 1] = '\0';
        mHostName = buffer;
    }
    mName = mHostName;
    
    mMsgHdl.registerCallback(REQ_ALLOC_SERVICE_ADDRESS, &CNameServer::onAllocServiceAddressReq);
    mMsgHdl.registerCallback(REQ_REGISTER_SERVICE, &CNameServer::onRegisterServiceReq);
    mMsgHdl.registerCallback(REQ_UNREGISTER_SERVICE, &CNameServer::onUnegisterServiceReq);

    mMsgHdl.registerCallback(REQ_QUERY_SERVICE, &CNameServer::onQueryServiceReq);
    mMsgHdl.registerCallback(REQ_QUERY_SERVICE_INTER_MACHINE, &CNameServer::onQueryServiceInterMachineReq);
    mMsgHdl.registerCallback(REQ_NS_QUERY_EXPORTABLE_SERVICE, &CNameServer::onQueryExportableServiceReq);

    mMsgHdl.registerCallback(REQ_QUERY_HOST_LOCAL, &CNameServer::onQueryHostReq);

    mSubscribeHdl.registerCallback(NTF_HOST_ONLINE_LOCAL, &CNameServer::onHostOnlineReg);

    mSubscribeHdl.registerCallback(NTF_HOST_INFO, &CNameServer::onHostInfoReg);

    mSubscribeHdl.registerCallback(NTF_WATCHDOG, &CNameServer::onWatchdogReg);

#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
    mLocalAllocator.setInterfaceIp(FDB_LOCAL_HOST);
    mLocalAllocator.setExportableLevel(FDB_EXPORTABLE_NODE_INTERNAL);
    mLocalAllocator.setInterfaceId(FDB_INVALID_ID);
#else
    mIPCAllocator.setExportableLevel(FDB_EXPORTABLE_NODE_INTERNAL);
    mIPCAllocator.setInterfaceId(FDB_INVALID_ID);
#endif

    mIntraNormalPublisher.bindToNs();
    mIntraMonitorPublisher.bindToNs();
    mInterNormalPublisher.bindToNs();
    mInterMonitorPublisher.bindToNs();

    mBindAddrPublisher.bindToNs();
}

CNameServer::~CNameServer()
{
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->prepareDestroy();
        delete proxy;
    }
}

void CNameServer::onSubscribe(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    const CFdbMsgSubscribeItem *sub_item;
    FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
    {
        mSubscribeHdl.processMessage(this, msg_ref, sub_item, sub_item->msg_code());
    }
    FDB_END_FOREACH_SIGNAL()
}

void CNameServer::onInvoke(CBaseJob::Ptr &msg_ref)
{
    mMsgHdl.processMessage(this, msg_ref);
}

bool CNameServer::checkExportable(const CFdbAddressDesc &addr_desc, int32_t max_exported_level)
{
    if (addr_desc.mExportableLevel > max_exported_level)
    {
        return false;
    }
    if (addr_desc.mExportableLevel == FDB_EXPORTABLE_NODE_INTERNAL)
    {
        if ((max_exported_level != FDB_EXPORTABLE_ANY) &&
            (max_exported_level != FDB_EXPORTABLE_NODE_INTERNAL))
        {
            return false;
        }
    }
    return true;
}

void CNameServer::populateAddressItem(const CFdbAddressDesc &addr_desc, FdbMsgAddressItem *item)
{
    item->fromSocketAddress(addr_desc.mAddress);
    item->set_secure(addr_desc.mAddress.mSecure);
    item->set_exportable_level(addr_desc.mExportableLevel);
    item->set_interface_id(addr_desc.mInterfaceId);

    if (addr_desc.mUDPPort != FDB_INET_PORT_INVALID)
    {
        item->set_udp_port(addr_desc.mUDPPort);
    }
}

void CNameServer::prepareAddress(const CFdbAddressDesc &addr_desc, FdbMsgAddressList &list,
                                 int32_t max_exported_level)
{
    if (!checkExportable(addr_desc, max_exported_level))
    {
        return;
    }
    populateAddressItem(addr_desc, list.add_address_list());
}

void CNameServer::prepareAddress(const CFdbAddressDesc &addr_desc, FdbMsgHostAddress &host_address,
                                 int32_t max_exported_level)
{
    if (!checkExportable(addr_desc, max_exported_level))
    {
        return;
    }
    populateAddressItem(addr_desc, host_address.add_address_list());
}

void CNameServer::populateAddrList(const tAddressDescTbl &addr_tbl, FdbMsgAddressList &list,
                                   EFdbSocketType type, int32_t max_exported_level)
{
    for (auto it = addr_tbl.begin(); it != addr_tbl.end(); ++it)
    {
        if ((type == FDB_SOCKET_MAX) || (type == it->mAddress.mType))
        {
            if (it->mStatus == CFdbAddressDesc::ADDR_BOUND)
            {
                prepareAddress(*it, list, max_exported_level);
            }
        }
    }
}

void CNameServer::populateNsAddrList(FdbMsgHostAddress &host_address)
{
    tRegistryMatchTbl reg_tbl;
    if (findService(CNsConfig::getNameServerName(), FDB_DEFAULT_INSTANCE, &reg_tbl))
    {
        auto entry = reg_tbl[0];
        auto &addr_tbl = entry->mAddrTbl;
        for (auto desc_it = addr_tbl.begin(); desc_it != addr_tbl.end(); ++desc_it)
        {
            if ((desc_it->mAddress.mType != FDB_SOCKET_IPC) &&
                    (desc_it->mStatus == CFdbAddressDesc::ADDR_BOUND) &&
                    (desc_it->mAddress.mAddr != FDB_LOCAL_HOST))
            {
                prepareAddress(*desc_it, host_address, mMaxDomainExportableLevel);
            }
        }
    }
}

void CNameServer::setServiceInfo(FdbMsgAddressList &addr_list,
                                 const char *svc_name, const char *endpoint_name,
                                 bool is_local, CBASE_tProcId pid,
                                 FdbInstanceId_t instance_id)
{
    addr_list.set_service_name(svc_name);
    addr_list.set_endpoint_name(endpoint_name);
    addr_list.set_host_name(mHostName);
    addr_list.set_domain_name(mDomainName);
    addr_list.set_is_local(is_local);
    addr_list.set_pid(pid);
    addr_list.set_instance_id(instance_id);
}

void CNameServer::setServiceInfo(FdbMsgAddressList &addr_list,
                                 const CSvcRegistryEntry &registry,
                                 bool is_local, CBASE_tProcId pid)
{
    setServiceInfo(addr_list, registry.mSvcName.c_str(), registry.mEndpointName.c_str(),
                   is_local, pid, registry.mInstanceId);
}

void CNameServer::populateExportAddrList(FdbMsgExportableSvcAddress &export_svc_addr_list)
{
    for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
    {
        for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
        {
            auto &reg_entry = it_id->second;
            FdbMsgAddressList *addr_list = 0;
            for (auto addr_it = reg_entry.mAddrTbl.begin(); addr_it != reg_entry.mAddrTbl.end(); ++addr_it)
            {
                auto &desc = *addr_it;
                if ((desc.mExportableLevel < mMinUpstreamExportableLevel) ||
                    (desc.mStatus != CFdbAddressDesc::ADDR_BOUND))
                {
                    continue;
                }
                if (!addr_list)
                {
                    addr_list = export_svc_addr_list.add_svc_address_list();
                    setServiceInfo(*addr_list, reg_entry, false, 0);
                }
                auto addr_item = addr_list->add_address_list();
                addr_item->fromSocketAddress(desc.mAddress);
                addr_item->set_udp_port(desc.mUDPPort);
                addr_item->set_exportable_level(desc.mExportableLevel);
                addr_item->set_interface_id(desc.mInterfaceId);
            }
        }
    }
}

void CNameServer::setHostInfo(CFdbSession *session, FdbMsgServiceInfo *msg_svc_info, const char *svc_name)
{
    std::string host_ip;
    if (!session->hostIp(host_ip))
    {
        host_ip = FDB_IP_ALL_INTERFACE;
    }
    msg_svc_info->host_addr().set_ip_address(host_ip);
    populateNsAddrList(msg_svc_info->host_addr());
    msg_svc_info->host_addr().set_host_name(mHostName);
}

void CNameServer::populateServerTable(CFdbSession *session, FdbMsgServiceTable &svc_tbl, bool is_local)
{
    if (mRegistryTbl.empty())
    {
        auto msg_svc_info = svc_tbl.add_service_tbl();
        setHostInfo(session, msg_svc_info, 0);
        setServiceInfo(msg_svc_info->service_addr(), "", "", is_local, 0);
    }
    else
    {
        for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
        {
            for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
            {
                auto &reg_entry = it_id->second;
                auto msg_svc_info = svc_tbl.add_service_tbl();
                setHostInfo(session, msg_svc_info, reg_entry.mSvcName.c_str());
                // lssvc only need to get UDP port of servers
                populateAddrList(reg_entry.mAddrTbl, msg_svc_info->service_addr(), FDB_SOCKET_MAX,
                                 FDB_EXPORTABLE_ANY);
                CBASE_tProcId pid = 0;
                auto svc_session = getSession(reg_entry.mSid);
                if (svc_session)
                {
                    pid = svc_session->pid();
                }
                else if (reg_entry.mSvcName == nsName())
                {
                    pid = CBaseThread::getPid();
                }
                setServiceInfo(msg_svc_info->service_addr(), reg_entry, is_local, pid);
            }
        }
    }

    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->dumpExportedSvcInfo(svc_tbl.exported_service_tbl());
    }
}

bool CNameServer::addressRegistered(const tAddressDescTbl &addr_list,
                                    CFdbSocketAddr &sckt_addr)
{
    for (auto it = addr_list.begin(); it != addr_list.end(); ++it)
    {
        auto &addr_desc = *it;
#if 0
        // Only two interface is allowed for each service: secure and normal
        if ((addr_desc.mAddress.mType == sckt_addr.mType) &&
             !addr_desc.mAddress.mAddr.compare(sckt_addr.mAddr) &&
             (addr_desc.mAddress.mSecure == sckt_addr.mSecure))
        {
            return true;
        }
 #else
        if (addr_desc.mAddress.mUrl == sckt_addr.mUrl)
        {
            return true;
        }
 #endif
    }
    return false;
}

void CNameServer::addOneServiceAddress(CSvcRegistryEntry &addr_tbl,
                                       bool inet_address,
                                       FdbMsgAddressList *msg_addr_list)
{
    tSocketAddrTbl sckt_addr_tbl;
    if (inet_address)
    {
        allocateTCPAddress(addr_tbl, sckt_addr_tbl);
    }
    else
    {
#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
        allocateTCPAddress(mLocalAllocator, addr_tbl, sckt_addr_tbl);
#else
        allocateIPCAddress(addr_tbl, sckt_addr_tbl);
#endif
    }

    for (auto it = sckt_addr_tbl.begin(); it != sckt_addr_tbl.end(); ++it)
    {
        if (addressRegistered(addr_tbl.mAddrTbl, it->mSocketAddr))
        {
            continue;
        }

        addr_tbl.mAddrTbl.resize(addr_tbl.mAddrTbl.size() + 1);
        auto &desc = addr_tbl.mAddrTbl.back();
        desc.mAddress = it->mSocketAddr;
        desc.mExportableLevel = it->mExportableLevel;
        desc.mInterfaceId = it->mInterfaceId;
        desc.mStatus = CFdbAddressDesc::ADDR_PENDING;
        if (desc.mAddress.mType != FDB_SOCKET_IPC)
        {
            allocateUDPPort(desc.mAddress.mAddr.c_str(), desc.mUDPPort);
        }
        if (msg_addr_list)
        {
            prepareAddress(desc, *msg_addr_list, FDB_EXPORTABLE_ANY);
        }
    }
}

bool CNameServer::addServiceAddress(CSvcRegistryEntry &addr_tbl,
                                    bool inet_only,
                                    FdbMsgAddressList *msg_addr_list)
{
    if (msg_addr_list)
    {
        CBASE_tProcId pid = (nsName() == addr_tbl.mSvcName) ? CBaseThread::getPid() : 0;
        setServiceInfo(*msg_addr_list, addr_tbl, true, pid);
    }

    if (!inet_only)
    {
        addOneServiceAddress(addr_tbl, false, msg_addr_list);
    }
    addOneServiceAddress(addr_tbl, true, msg_addr_list);

    return msg_addr_list ? !msg_addr_list->address_list().empty() : false;
}

void CNameServer::onAllocServiceAddressReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgServerName alloc_req;
    auto sid = msg->session();
    CFdbParcelableParser parser(alloc_req);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    const char *svc_name = alloc_req.name().c_str();
    auto instance_id = alloc_req.instance_id();
    if (fdbIsGroup(instance_id))
    {
        bool wrap = false;
        while (1)
        {
            auto next_id = mInstanceIdAllocator++;
            if (next_id > FDB_AUTO_INSTANCE_END)
            {
                mInstanceIdAllocator = FDB_AUTO_INSTANCE_BEGIN;
                if (wrap)
                {
                    LOG_E("CNameServer: service %s: unable to allocate instance!\n", svc_name);
                    msg->status(msg_ref, FDB_ST_NO_RESOURCE);
                    return;
                }
                wrap = true;
                continue;
            }
            else
            {
                instance_id = fdbMergeEventCode(instance_id, next_id);
            }

            if (!findService(svc_name, instance_id))
            {
                break;
            }
        }
    }
    else if (findService(svc_name, instance_id))
    {
        msg->status(msg_ref, FDB_ST_ALREADY_EXIST);
        return;
    }

    FdbMsgAddressList reply_addr_list;
    auto &reg_entry = mRegistryTbl[svc_name][instance_id];
    CFdbToken::allocateToken(reg_entry.mTokens);
    reply_addr_list.populateTokens(reg_entry.mTokens);
    reg_entry.setSvcName(svc_name);
    reg_entry.mSid = sid;
    reg_entry.mEndpointName = alloc_req.endpoint_name();
    auto flag = alloc_req.flag();
    reg_entry.mAllowTcpSecure = !!(flag & FDB_NS_ALLOW_TCP_SECURE);
    reg_entry.mAllowTcpNormal = !!(flag & FDB_NS_ALLOW_TCP_NORMAL);
    reg_entry.mExportableLevel = alloc_req.exportable_level();
    reg_entry.mInstanceId = instance_id;

    addServiceAddress(reg_entry, false, &reply_addr_list);

    if (reply_addr_list.address_list().empty())
    {
        msg->status(msg_ref, FDB_ST_NOT_AVAILABLE);
        return;
    }

    CFdbParcelableBuilder builder(reply_addr_list);
    msg->reply(msg_ref, builder);
}

void CNameServer::populateTokensRemote(const CFdbToken::tTokenList &tokens,
                                       FdbMsgAddressList &addr_list,
                                       CFdbSession *session)
{
    addr_list.token_list().clear_tokens();
    // send token matching security level to local clients
    int32_t security_level = session->securityLevel();
    if ((security_level >= 0) && (int32_t)tokens.size())
    {
        if (security_level >= (int32_t)tokens.size())
        {
            security_level = (int32_t)tokens.size() - 1; 
        }
        // only give the tokens matching secure level of the host
        for (int32_t i = 0; i <= security_level; ++i) 
        {
            addr_list.token_list().add_tokens(tokens[i].c_str());
        }    
    }    
}

void CNameServer::broadcastSvcAddrRemote(const CFdbToken::tTokenList &tokens,
                                         FdbMsgAddressList &addr_list,
                                         CFdbMessage *msg)
{
    auto session = msg->getSession();
    if (session)
    {    
        populateTokensRemote(tokens, addr_list, session);
        CFdbParcelableBuilder builder(addr_list);
        msg->broadcast(addr_list.instance_id(), builder, addr_list.service_name().c_str());
        addr_list.token_list().clear_tokens();
    }
}

void CNameServer::broadcastSvcAddrRemote(const CFdbToken::tTokenList &tokens,
                                         FdbMsgAddressList &addr_list,
                                         CFdbSession *session)
{
    populateTokensRemote(tokens, addr_list, session);
    CFdbParcelableBuilder builder(addr_list);
    mInterNormalPublisher.broadcast(session->sid(), addr_list.instance_id(), builder,
                                    addr_list.service_name().c_str());
    addr_list.token_list().clear_tokens();
}

void CNameServer::broadcastSvcAddrRemote(const CFdbToken::tTokenList &tokens,
                                         FdbMsgAddressList &addr_list)
{
    tSubscribedSessionSets sessions;
    mInterNormalPublisher.getSubscribeTable(addr_list.instance_id(),
                                             addr_list.service_name().c_str(),
                                             sessions);
    for (tSubscribedSessionSets::iterator it = sessions.begin(); it != sessions.end(); ++it)
    {
        CFdbSession *session = *it;
        broadcastSvcAddrRemote(tokens, addr_list, session);
    }
}

void CNameServer::populateTokensLocal(const CFdbToken::tTokenList &tokens,
                                      FdbMsgAddressList &addr_list,
                                      CFdbSession *session)
{
    auto svc_name = addr_list.service_name().c_str();
    addr_list.token_list().clear_tokens();
    // send token matching security level to local clients
    int32_t security_level = getSecurityLevel(session, svc_name);
    if ((security_level >= 0) && (int32_t)tokens.size())
    {
        if (security_level >= (int32_t)tokens.size())
        {
            security_level = (int32_t)tokens.size() - 1; 
        }
        addr_list.token_list().add_tokens(tokens[security_level].c_str());
    }    
}

void CNameServer::broadcastSvcAddrLocal(const CFdbToken::tTokenList &tokens,
                                        FdbMsgAddressList &addr_list,
                                        CFdbMessage *msg)
{
    auto session = msg->getSession();
    if (session)
    {    
        populateTokensLocal(tokens, addr_list, session);
        allocateUDPPortForClients(addr_list);
        
        CFdbParcelableBuilder builder(addr_list);
        msg->broadcast(addr_list.instance_id(), builder, addr_list.service_name().c_str());
        addr_list.token_list().clear_tokens();
    }
}

void CNameServer::broadcastSvcAddrLocal(const CFdbToken::tTokenList &tokens,
                                        FdbMsgAddressList &addr_list,
                                        CFdbSession *session)
{
    populateTokensLocal(tokens, addr_list, session);
    allocateUDPPortForClients(addr_list);

    CFdbParcelableBuilder builder(addr_list);
    mIntraNormalPublisher.broadcast(session->sid(), addr_list.instance_id(), builder,
                                    addr_list.service_name().c_str());
    addr_list.token_list().clear_tokens();
}

void CNameServer::broadcastSvcAddrLocal(const CFdbToken::tTokenList &tokens,
                                        FdbMsgAddressList &addr_list)
{
    tSubscribedSessionSets sessions;
    mIntraNormalPublisher.getSubscribeTable(addr_list.instance_id(),
                                             addr_list.service_name().c_str(),
                                             sessions);
    for (auto it = sessions.begin(); it != sessions.end(); ++it)
    {
        auto session = *it;
        broadcastSvcAddrLocal(tokens, addr_list, session);
    }
}

bool CNameServer::hasPendingAddress(const CSvcRegistryEntry &reg_entry)
{
    for (auto it = reg_entry.mAddrTbl.begin(); it != reg_entry.mAddrTbl.end(); ++it)
    {
        if (it->mStatus == CFdbAddressDesc::ADDR_PENDING)
        {
            return true;
        }
    }
    return false;
}

void CNameServer::prepareAddressesToNotify(std::string &best_local_url, CSvcRegistryEntry &reg_entry,
                                           FdbMsgAddressList &local_addr_list,
                                           FdbMsgAddressList &remote_tcp_addr_list,
                                           FdbMsgAddressList &all_addr_list)
{
    setServiceInfo(local_addr_list, reg_entry, true, 0);
    setServiceInfo(remote_tcp_addr_list, reg_entry, false, 0);
    setServiceInfo(all_addr_list, reg_entry, true, 0);

    CFdbAddressDesc *best_local_ipc = 0;
    typedef std::vector<CFdbAddressDesc *> tTcpDescriptors;
    tTcpDescriptors best_local_host_tcp;
    tTcpDescriptors best_all_interface_tcp;

    for (auto it = reg_entry.mAddrTbl.begin(); it != reg_entry.mAddrTbl.end(); ++it)
    {
        auto &desc = *it;
        prepareAddress(desc, all_addr_list, FDB_EXPORTABLE_ANY);
        if (desc.mAddress.mType == FDB_SOCKET_IPC)
        {
            best_local_ipc = &desc;
        }
        else
        {
            if (desc.mAddress.mAddr == FDB_LOCAL_HOST)
            {
                best_local_host_tcp.push_back(&desc);
            }
            else
            {
                prepareAddress(desc, remote_tcp_addr_list, mMaxDomainExportableLevel);
                if (desc.mAddress.mAddr == FDB_IP_ALL_INTERFACE)
                {
                    best_all_interface_tcp.push_back(&desc);
                }
            }
        }
    }

    std::string best_ipc_url;
    std::string best_tcp_normal_url;
    std::string best_tcp_secure_url;
    if (best_local_ipc)
    {
        prepareAddress(*best_local_ipc, local_addr_list, FDB_EXPORTABLE_NODE_INTERNAL);
        best_ipc_url = best_local_ipc->mAddress.mUrl;
    }
    else if (!best_local_host_tcp.empty())
    {
        for (auto it = best_local_host_tcp.begin(); it != best_local_host_tcp.end(); ++it)
        {
            auto desc = *it; // get pointer
            prepareAddress(*desc, local_addr_list, FDB_EXPORTABLE_NODE_INTERNAL);
            if (desc->mAddress.mSecure)
            {
                best_tcp_secure_url = desc->mAddress.mUrl;
            }
            else
            {
                best_tcp_normal_url = desc->mAddress.mUrl;
            }
        }
    }
    else
    {
        for (auto it = best_all_interface_tcp.begin(); it != best_all_interface_tcp.end(); ++it)
        {
            auto desc = *(*it); // deep copy
            desc.mAddress.mAddr = FDB_LOCAL_HOST;
            desc.mAddress.buildUrl();
            prepareAddress(desc, local_addr_list, FDB_EXPORTABLE_DOMAIN);
            if (desc.mAddress.mSecure)
            {
                if (best_tcp_secure_url.empty())
                {
                    best_tcp_secure_url = desc.mAddress.mUrl;
                }
            }
            else
            {
                if (best_tcp_normal_url.empty())
                {
                    best_tcp_normal_url = desc.mAddress.mUrl;
                }
            }
        }
    }

    all_addr_list.token_list().clear_tokens(); // never broadcast token to monitors!!!

    if (!best_ipc_url.empty())
    {
        best_local_url = best_ipc_url;
    }
    else if (!best_tcp_secure_url.empty())
    {
        best_local_url = best_tcp_secure_url;
    }
    else if (!best_tcp_normal_url.empty())
    {
        best_local_url = best_tcp_normal_url;
    }
}

void CNameServer::onRegisterServiceReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgAddrBindResults addr_list;
    CFdbParcelableParser parser(addr_list);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }
    const std::string &svc_name = addr_list.service_name();
    auto instance_id = addr_list.instance_id();

    if (fdbIsGroup(instance_id))
    {
        LOG_E("CNameServer: service %s:%d: register bad instance id!\n",
              svc_name.c_str(), instance_id);
        msg->status(msg_ref, FDB_ST_BAD_PARAMETER);
        return;
    }

    tRegistryMatchTbl reg_tbl;
#if 1
    if (!findService(svc_name.c_str(), instance_id, &reg_tbl))
    {
        LOG_E("CNameServer: service %s:%d: not authenticated!\n",
              svc_name.c_str(), instance_id);
        msg->status(msg_ref, FDB_ST_NON_EXIST);
        return;
    }
#else
    if (reg_it == mRegistryTbl.end())
    {
        CSvcRegistryEntry &addr_tbl = mRegistryTbl[addr_list.service_name()];
        reg_it = mRegistryTbl.find(svc_name);
    }
#endif
    if (reg_tbl.size() > 1)
    {
        //TODO: this shall not happen!!!
    }
    auto reg_entry = reg_tbl[0];

    auto &addrs = addr_list.address_list();
    if (addrs.pool().empty())
    {
        eraseService(svc_name.c_str(), instance_id);
        LOG_E("CNameServer: Fail to register service %s!\n", svc_name.c_str());
        return;
    }

    reg_entry->mSid = msg->session();
    tAddressDescTbl new_addr_tbl;
    for (auto msg_it = addrs.pool().begin(); msg_it != addrs.pool().end(); ++msg_it)
    {
        CFdbAddressDesc *desc = 0;
        tAddressDescTbl::iterator desc_it = reg_entry->mAddrTbl.end();
        for (auto addr_it = reg_entry->mAddrTbl.begin();
                addr_it != reg_entry->mAddrTbl.end(); ++addr_it)
        {
            auto &addr_in_tbl = addr_it->mAddress;
            if (!msg_it->request_address().compare(addr_in_tbl.mUrl))
            {
                desc = &(*addr_it);
                if (msg_it->bind_address().empty())
                {
                    desc_it = addr_it;
                }
                else {
                    desc->mStatus = CFdbAddressDesc::ADDR_BOUND;
                    desc->mUDPPort = msg_it->udp_port();
                    if ((addr_in_tbl.mType != FDB_SOCKET_IPC) && 
                            msg_it->bind_address().compare(addr_in_tbl.mUrl))
                    {
                        CFdbSocketAddr addr;
                        if (CBaseSocketFactory::parseUrl(msg_it->bind_address().c_str(), addr))
                        {
                            desc->mAddress = addr;
                        }
                    }
                }
                break;
            }
        }

        if (!desc)
        {
            new_addr_tbl.resize(new_addr_tbl.size() + 1);
            desc = &new_addr_tbl.back();
            if (CBaseSocketFactory::parseUrl(msg_it->bind_address().c_str(), desc->mAddress))
            {
                desc->mStatus = CFdbAddressDesc::ADDR_BOUND;
            }
            else
            {
                new_addr_tbl.pop_back();
                desc = 0;
            }
        }

        if (desc)
        {
            if (desc->mStatus != CFdbAddressDesc::ADDR_BOUND)
            {
                if (!rebindToAddress(desc, *reg_entry))
                {
                    if (desc_it != reg_entry->mAddrTbl.end())
                    {
                        reg_entry->mAddrTbl.erase(desc_it);
                    }
                }
            }
        }
    }
    if (!new_addr_tbl.empty())
    {
        reg_entry->mAddrTbl.insert(reg_entry->mAddrTbl.end(), new_addr_tbl.begin(), new_addr_tbl.end());
    }

    if (hasPendingAddress(*reg_entry))
    {
        LOG_I("CNameServer: Service %s: registry fails.\n", svc_name.c_str());
        return;
    }
    else
    {
        LOG_I("CNameServer: Service %s (%s) is registered.\n", svc_name.c_str(), reg_entry->mEndpointName.c_str());
    }

    std::string best_local_url;
    FdbMsgAddressList local_addr_list;
    FdbMsgAddressList remote_tcp_addr_list;
    FdbMsgAddressList all_addr_list;
    prepareAddressesToNotify(best_local_url, *reg_entry, local_addr_list,
                             remote_tcp_addr_list, all_addr_list);

    if (svc_name == CNsConfig::getHostServerName())
    {
        if (best_local_url.empty())
        {
            LOG_E("CNameServer: Unable to retrieve address of host server!\n");
        }
        else
        {
            connectToHostServer(best_local_url.c_str(), true);
        }
    }

    if (!all_addr_list.address_list().empty())
    {
        all_addr_list.set_is_local(true);
        {
        CFdbParcelableBuilder builder(all_addr_list);
        mIntraMonitorPublisher.broadcast(instance_id, builder, svc_name.c_str());
        }
        all_addr_list.set_is_local(false);
        {
        CFdbParcelableBuilder builder(all_addr_list);
        mInterMonitorPublisher.broadcast(instance_id, builder, svc_name.c_str());
        }
    }
    if (!local_addr_list.address_list().empty())
    {
        broadcastSvcAddrLocal(reg_entry->mTokens, local_addr_list);
    }
    if (!remote_tcp_addr_list.address_list().empty())
    {
        // send all tokens to the local name server, which will send appropiate
        // token to local clients according to security level
        broadcastSvcAddrRemote(reg_entry->mTokens, remote_tcp_addr_list);
    }

    FdbMsgExportableSvcAddress export_addr_tbl;
    populateExportAddrList(export_addr_tbl);
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->postSvcTable(export_addr_tbl);
    }
}

bool CNameServer::rebindToAddress(CFdbAddressDesc *addr_desc, const CSvcRegistryEntry &addr_tbl)
{
    if (addr_desc->mRebindCount >= CNsConfig::getAddressBindRetryCnt())
    {
        LOG_E("CNameServer: Service %s: fail to bind address.\n", addr_tbl.mSvcName.c_str());
        addr_desc->mRebindCount = 0;
        return false;
    }
    addr_desc->mRebindCount++;

    int32_t udp_port = FDB_INET_PORT_INVALID;

    // allocate another address
    IAddressAllocator *allocator;
    tTCPAllocatorTbl allocator_tbl;
    if (addr_desc->mAddress.mType == FDB_SOCKET_IPC)
    {
#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
        addr_desc->mRebindCount = 0;
        return false;
#else
        allocator = &mIPCAllocator;
#endif
    }
    else
    {
        allocator = getTCPAllocator(addr_desc->mAddress.mAddr);
        if (!allocator && (addr_tbl.mSvcName == CNsConfig::getHostServerName()))
        {
            createTCPAllocator(allocator_tbl);
            allocator = getTCPAllocator(allocator_tbl, addr_desc->mAddress.mAddr);
        }

        if (!allocator)
        {
            LOG_E("CNameServer: Service %s: unable to allocate address for IP %s\n", addr_tbl.mSvcName.c_str(),
                  addr_desc->mAddress.mAddr.c_str());
            addr_desc->mRebindCount = 0;
            return false;
        }
        allocateUDPPort(addr_desc->mAddress.mAddr.c_str(), udp_port);
    }
    CAllocatedAddress allocated_addr;
    CFdbSocketAddr &sckt_addr = allocated_addr.mSocketAddr;
    bool ret = allocateAddress(*allocator, addr_tbl, allocated_addr, addr_desc->mAddress.mSecure);
    addr_desc->mExportableLevel = allocated_addr.mExportableLevel;
    addr_desc->mInterfaceId = allocated_addr.mInterfaceId;
    if (ret && CBaseSocketFactory::parseUrl(sckt_addr.mUrl.c_str(), addr_desc->mAddress))
    {
        addr_desc->mUDPPort = udp_port;
        // replace failed address with a new one
        FdbMsgAddressList addr_list;
        auto item = addr_list.add_address_list();
        item->fromSocketAddress(sckt_addr);
        item->set_secure(addr_desc->mAddress.mSecure);
        item->set_exportable_level(addr_desc->mExportableLevel);
        item->set_interface_id(addr_desc->mInterfaceId);
        if (udp_port != FDB_INET_PORT_INVALID)
        {
            item->set_udp_port(udp_port);
        }
        setServiceInfo(addr_list, addr_tbl, true, 0);;

        CFdbParcelableBuilder builder(addr_list);
        mBindAddrPublisher.broadcast(addr_list.instance_id(), builder, addr_tbl.mSvcName.c_str());
        LOG_E("CNameServer: Service %s: fail to bind address and retry...\n", addr_tbl.mSvcName.c_str());
        return true;
    }

    addr_desc->mRebindCount = 0;
    return false;
}

void CNameServer::notifyRemoteNameServerDrop(const char *host_name)
{
    FdbMsgAddressList broadcast_addr_list;
    auto svc_name = CNsConfig::getNameServerName();

    setServiceInfo(broadcast_addr_list, nsName().c_str(), name().c_str(), false, 0);

    CFdbParcelableBuilder builder(broadcast_addr_list);
    mIntraMonitorPublisher.broadcast(FDB_DEFAULT_INSTANCE, builder, svc_name);
}

void CNameServer::onHostOnline(bool online, CIntraHostProxy *host_proxy)
{
    if (!online)
    {
        return;
    }

    if (!mTCPAllocators.empty())
    {
        return;
    }
    if (host_proxy->local())
    {
        mDomainName = host_proxy->domainName();
    }

    // allocate TCP address upon online of the first host server
    createTCPAllocator();
    for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
    {
        for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
        {
            auto &reg_entry = it_id->second;
            if (!reg_entry.mSvcName.compare(CNsConfig::getHostServerName()))
            {
                continue;
            }
            FdbMsgAddressList net_addr_list;
            if (addServiceAddress(reg_entry, true, &net_addr_list))
            {
                if (reg_entry.mSvcName.compare(nsName()))
                {
                    CFdbParcelableBuilder builder(net_addr_list);
                    mBindAddrPublisher.broadcast(reg_entry.mInstanceId, builder, reg_entry.mSvcName.c_str());
                }
                else
                {
                    bindNsAddress(reg_entry.mAddrTbl);
                }
            }
        }
    }
}

bool CNameServer::removeService(const char *svc_name, FdbInstanceId_t instance_id)
{
    tRegistryMatchTbl reg_tbl;
    if (!findService(svc_name, instance_id, &reg_tbl))
    {
        return false;
    }

    if (reg_tbl.size() > 1)
    {
        //TODO: this shall not happen!
    }
    auto reg_entry = reg_tbl[0];

    LOG_I("CNameServer: Service %s is unregistered.\n", svc_name);
    FdbMsgAddressList broadcast_addr_list;
    setServiceInfo(broadcast_addr_list, *reg_entry, true, 0);

    broadcastSvcAddrLocal(reg_entry->mTokens, broadcast_addr_list);
    broadcast_addr_list.token_list().clear_tokens(); // never broadcast token to monitors!!!
    {
    CFdbParcelableBuilder builder(broadcast_addr_list);
    mIntraMonitorPublisher.broadcast(instance_id, builder, svc_name);
    }

    broadcast_addr_list.set_is_local(false);
    // send all tokens to the local name server, which will send appropiate
    // token to local clients according to security level
    broadcastSvcAddrRemote(reg_entry->mTokens, broadcast_addr_list);
    broadcast_addr_list.token_list().clear_tokens(); // never broadcast token to monitors!!!
    {
    CFdbParcelableBuilder builder(broadcast_addr_list);
    mInterMonitorPublisher.broadcast(instance_id, builder, svc_name);
    }
    bool unbind_exported_address = false;
    for (auto it = reg_entry->mAddrTbl.begin(); it != reg_entry->mAddrTbl.end(); ++it)
    {
        if ((it->mExportableLevel > FDB_EXPORTABLE_NODE_INTERNAL) && 
            (it->mStatus == CFdbAddressDesc::ADDR_BOUND))
        {
            unbind_exported_address = true;
            break;
        }
    }
    bool ret = eraseService(svc_name, instance_id);

    if (unbind_exported_address)
    {
        FdbMsgExportableSvcAddress export_addr_tbl;
        populateExportAddrList(export_addr_tbl);
        for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
        {
            auto proxy = it->second;
            proxy->postSvcTable(export_addr_tbl);
        }
    }

    return ret;
}

void CNameServer::onUnegisterServiceReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgServerName msg_svc_name;
    CFdbParcelableParser parser(msg_svc_name);
    if (!msg->deserialize(parser))
    {
        msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
        return;
    }

    auto instance_id = msg_svc_name.instance_id();
    if (fdbIsGroup(instance_id))
    {
        LOG_E("CNameServer: service %s:%d: unregister bad instance id!\n",
              msg_svc_name.name().c_str(), instance_id);
        msg->status(msg_ref, FDB_ST_BAD_PARAMETER);
        return;
    }

    removeService(msg_svc_name.name().c_str(), instance_id);
}

void CNameServer::onQueryServiceReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    auto session = msg->getSession();
    auto pending_req = new tPendingServiceReqTbl;
    auto svc_tbl = new FdbMsgServiceTable;

    populateServerTable(session, *svc_tbl, true);
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->prepareQueryService(pending_req, svc_tbl);
    }
    mStaticNameProxy.prepareQueryService(pending_req, svc_tbl);
    bool sent = false;
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        if (proxy->sendQueryServiceReq(msg_ref, pending_req, svc_tbl))
        {
            sent = true;
        }
    }
    if (mStaticNameProxy.sendQueryServiceReq(msg_ref, pending_req, svc_tbl))
    {
        sent = true;
    }
    if (!sent)
    {
        CFdbParcelableBuilder builder(*svc_tbl);
        msg->reply(msg_ref, builder);
        delete pending_req;
        delete svc_tbl;
    }
}

void CNameServer::finalizeServiceQuery(FdbMsgServiceTable *svc_tbl, CFdbMessage *msg)
{
    auto query = static_cast<CQueryServiceMsg *>(msg);
    if (svc_tbl)
    {
        const auto &src_pool = svc_tbl->service_tbl().pool();
        auto &dst_pool = query->mSvcTbl->service_tbl().vpool();
        dst_pool.insert(dst_pool.end(), src_pool.begin(), src_pool.end());
    }
    fdb_remove_value_from_container(*(query->mPendingReqTbl), query->mHostIp);
    
    if (query->mPendingReqTbl->empty())
    {
        auto msg = castToMessage<CFdbMessage *>(query->mReq);
        CFdbParcelableBuilder builder(*query->mSvcTbl);
        msg->reply(query->mReq, builder);
        delete query->mPendingReqTbl;
        delete query->mSvcTbl;
    }
}

void CNameServer::onQueryServiceInterMachineReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgServiceTable svc_tbl;
    auto session = msg->getSession();
    populateServerTable(session, svc_tbl, false);
    CFdbParcelableBuilder builder(svc_tbl);
    msg->reply(msg_ref, builder);
}

void CNameServer::onQueryExportableServiceReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    auto pending_req = new tPendingServiceReqTbl;
    auto svc_tbl = new FdbMsgAllExportableService;
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        pending_req->push_back(it->first);
    }
    bool sent = false;
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        auto req = new CQueryExportedServiceMsg(pending_req, svc_tbl, msg_ref, it->first, this);
        if (proxy->sendQueryExportableServiceReq(req))
        {
            sent = true;
        }
    }
    if (!sent)
    {
        CFdbParcelableBuilder builder(*svc_tbl);
        msg->reply(msg_ref, builder);
        delete pending_req;
        delete svc_tbl;
    }
}

void CNameServer::finalizeExportableServiceQuery(FdbMsgAllExportableService *svc_tbl,
                                                 CFdbMessage *msg)
{
    auto query = static_cast<CQueryExportedServiceMsg *>(msg);
    if (svc_tbl)
    {
        {
        const auto &src_pool = svc_tbl->upstream_table().svc_address_list().pool();
        auto &dst_pool = query->mSvcTbl->upstream_table().svc_address_list().vpool();
        dst_pool.insert(dst_pool.end(), src_pool.begin(), src_pool.end());
        }
        {
        const auto &src_pool = svc_tbl->local_table().svc_address_list().pool();
        auto &dst_pool = query->mSvcTbl->local_table().svc_address_list().vpool();
        dst_pool.insert(dst_pool.end(), src_pool.begin(), src_pool.end());
        }
        {
        const auto &src_pool = svc_tbl->downstream_table().svc_address_list().pool();
        auto &dst_pool = query->mSvcTbl->downstream_table().svc_address_list().vpool();
        dst_pool.insert(dst_pool.end(), src_pool.begin(), src_pool.end());
        }
    }
    fdb_remove_value_from_container(*(query->mPendingReqTbl), query->mHostServerIp);
    
    if (query->mPendingReqTbl->empty())
    {
        auto msg = castToMessage<CFdbMessage *>(query->mReq);
        CFdbParcelableBuilder builder(*query->mSvcTbl);
        msg->reply(query->mReq, builder);
        delete query->mPendingReqTbl;
        delete query->mSvcTbl;
    }
}

void CNameServer::onQueryHostReq(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostAddressList host_tbl;
    getHostTbl(host_tbl, getSession(msg->session()));
    CFdbParcelableBuilder builder(host_tbl);
    msg->reply(msg_ref, builder);
}

void CNameServer::broadServiceAddress(const CSvcRegistryEntry &reg_entry, CFdbMessage *msg,
                                      SubscribeType subscribe_type)
{
    if ((subscribe_type == INTER_NORMAL) && (reg_entry.mSvcName == nsName()))
    {
        /* never broadcast name server to remote for normal request! */
        return;
    }

    if (hasPendingAddress(reg_entry))
    {
        return;
    }

    FdbMsgAddressList addr_list;
    EFdbSocketType skt_type;
    int32_t max_exported_level;
    if ((subscribe_type == INTRA_MONITOR) || (subscribe_type == INTER_MONITOR))
    {
        skt_type = FDB_SOCKET_MAX;
        max_exported_level = FDB_EXPORTABLE_ANY;
    }
    else
    {   // do not send external address to local clients
        skt_type = getSocketType(msg->session());
        max_exported_level = (subscribe_type == INTRA_NORMAL) ?
                             FDB_EXPORTABLE_NODE_INTERNAL : mMaxDomainExportableLevel;
    }

    populateAddrList(reg_entry.mAddrTbl, addr_list, skt_type, max_exported_level);
    if ((skt_type == FDB_SOCKET_IPC) && addr_list.address_list().empty())
    {   /* for request from local, fallback to TCP connection */
        populateAddrList(reg_entry.mAddrTbl, addr_list, FDB_SOCKET_TCP, mMaxDomainExportableLevel);
    }

    if (addr_list.address_list().empty())
    {
        return;
    }
    
    bool is_local;
    if ((subscribe_type == INTER_NORMAL) || (subscribe_type == INTER_MONITOR))
    {
        is_local = false;
    }
    else
    {
        is_local = true;
    }
    setServiceInfo(addr_list, reg_entry, is_local, 0);

    if (subscribe_type == INTRA_NORMAL)
    {
        broadcastSvcAddrLocal(reg_entry.mTokens, addr_list, msg);
    }
    else if (subscribe_type == INTER_NORMAL)
    {
        broadcastSvcAddrRemote(reg_entry.mTokens, addr_list, msg);
    }
    else
    {
        CFdbParcelableBuilder builder(addr_list);
        msg->broadcast(reg_entry.mInstanceId, builder, reg_entry.mSvcName.c_str());
    }
}

void CNameServer::onServiceOnlineReg(const char *svc_name, FdbInstanceId_t instance_id,
                                     CBaseJob::Ptr &msg_ref, SubscribeType subscribe_type)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);

    tRegistryMatchTbl reg_tbl;
    if (findService(svc_name, instance_id, &reg_tbl))
    {
        for (auto it = reg_tbl.begin(); it != reg_tbl.end(); ++it)
        {
            auto reg_entry = *it;
            broadServiceAddress(*reg_entry, msg, subscribe_type);
        }
    }

    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->broadcastExternalService(false, subscribe_type, instance_id, svc_name, msg);
    }

    if ((subscribe_type == INTER_NORMAL) || (subscribe_type == INTER_MONITOR))
    {
        LOG_I("CNameServer: Registry of %s is received from remote.\n",
                svc_name[0] == '\0' ? "all services" : svc_name);
    }
    else if (nsName() != svc_name)
    {
        for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
        {
            auto proxy = it->second;
            proxy->forwardServiceListener(subscribe_type, instance_id, svc_name, msg_ref);
        }
        mStaticNameProxy.forwardServiceListener(subscribe_type, instance_id, svc_name, msg_ref);
    }
}

void CNameServer::onHostOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostAddressList host_tbl;
    getHostTbl(host_tbl, getSession(msg->session()));
    CFdbParcelableBuilder builder(host_tbl);
    msg->broadcast(sub_item->msg_code(), builder);
}

void CNameServer::onHostInfoReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    FdbMsgHostInfo msg_host_info;
    msg_host_info.set_name(mHostName);
    CFdbParcelableBuilder builder(msg_host_info);
    msg->broadcast(sub_item->msg_code(), builder);
}

void CNameServer::onWatchdogReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    CFdbMsgProcessList process_list;
    getDroppedProcesses(process_list);
    CFdbParcelableBuilder builder(process_list);
    msg->broadcast(sub_item->msg_code(), builder);
}

CNameServer::CFdbAddressDesc *CNameServer::findAddress(EFdbSocketType type, const char *url)
{
    for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
    {
        for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
        {
            auto &reg_entry = it_id->second;
            for (auto desc_it = reg_entry.mAddrTbl.begin();
                    desc_it != reg_entry.mAddrTbl.end(); ++desc_it)
            {
                if ((type == FDB_SOCKET_MAX) || (desc_it->mAddress.mType == type))
                {
                    if (desc_it->mAddress.mUrl == url)
                    {
                        return &(*desc_it);
                    }
                }
            }
        }
    }
    return 0;
}

CNameServer::CFdbAddressDesc *CNameServer::findUDPPort(const char *ip_address, int32_t port)
{
    for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
    {
        for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
        {
            auto &reg_entry = it_id->second;
            for (auto desc_it = reg_entry.mAddrTbl.begin();
                    desc_it != reg_entry.mAddrTbl.end(); ++desc_it)
            {
                if (desc_it->mAddress.mType != FDB_SOCKET_IPC)
                {
                    if (!desc_it->mAddress.mAddr.compare(ip_address) &&
                            (desc_it->mUDPPort > 0) && (desc_it->mUDPPort == port))
                    {
                        return &(*desc_it);
                    }
                }
            }
        }
    }
    return 0;
}

bool CNameServer::allocateUDPPort(const char *ip_address, int32_t &port)
{
    // Actually we don't know which interface is used by client to connect with
    // server. So we have to use a global allocator to allocate UDP port for
    // all interface.
#if 0
    auto &allocator = mUDPAllocators[ip_address];
#else
    auto &allocator = mUDPAllocators[FDB_IP_ALL_INTERFACE];
#endif
    
    int32_t retries =  (int32_t)mRegistryTbl.size() + 8; // 8 is just come to me...
    while (--retries > 0)
    {
        int32_t p = allocator.allocate();
        if (!findUDPPort(ip_address, p))
        {
            port = p;
            return true;
        }
    }
    return false;
}

void CNameServer::allocateUDPPortForClients(FdbMsgAddressList &addr_list)
{
    auto &addrs = addr_list.address_list();
    for (auto it = addrs.vpool().begin(); it != addrs.vpool().end(); ++it)
    {
        if (it->address_type() == FDB_SOCKET_IPC)
        {
            continue;
        }
        int32_t udp_port;
        if (allocateUDPPort(it->tcp_ipc_address().c_str(), udp_port))
        {
            it->set_udp_port(udp_port);
        }
    }
}

bool CNameServer::allocateAddress(IAddressAllocator &allocator, const CSvcRegistryEntry &addr_tbl,
                                  CAllocatedAddress &allocated_addr, bool secure)
{
    CFdbSocketAddr &sckt_addr = allocated_addr.mSocketAddr;
    int32_t retries =  (int32_t)mRegistryTbl.size() + 8; // 8 is just come to me...
    while (--retries > 0)
    {
        allocator.allocate(allocated_addr, addr_tbl.mSvcType, secure);
        if ((sckt_addr.mType == FDB_SOCKET_TCP) && (sckt_addr.mPort == FDB_INET_PORT_AUTO))
        {
            return true;
        }
        if (addr_tbl.mSvcType != FDB_SVC_USER)
        {
            return true;
        }
        if (!findAddress(sckt_addr.mType, sckt_addr.mUrl.c_str()))
        {
            return true;
        }
    }

    LOG_E("NameServer: Fatal Error! allocator goes out of range!\n");
    return false;
}

void CNameServer::createTCPAllocator(tTCPAllocatorTbl &allocator_tbl)
{
    CFdbInterfaceTable interfaces_map;
    CBaseSocketFactory::getIpAddress(interfaces_map);
    for (auto it = mNetInterfaces.begin(); it != mNetInterfaces.end(); ++it)
    {
        const std::string *net_ip_address = 0;
        const std::string *net_mask = 0;
        auto &net_if = *it;

        const CFdbInterfaceTable::IfDesc *if_desc = net_if.isIfName ?
                                                    interfaces_map.findByName(net_if.mAddress.c_str()) :
                                                    interfaces_map.findByIp(net_if.mAddress.c_str());
        if (if_desc)
        {
            net_ip_address = &if_desc->mIpAddr;
            net_mask = &if_desc->mMask;
        }
        else if (!net_if.isIfName)
        {
            net_ip_address = &net_if.mAddress;
        }

        if (net_ip_address)
        {
#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
            if (*net_ip_address == FDB_LOCAL_HOST)
            {   // FDB_LOCAL_HOST will be allocated at local allocator.
                continue;
            }
#endif
            auto &allocator = allocator_tbl[*net_ip_address];
            allocator.setInterfaceIp(net_ip_address->c_str());
            if (net_mask)
            {
                allocator.setMask(net_mask->c_str());
            }
            allocator.setExportableLevel(net_if.mExportableLevel);
            allocator.setInterfaceId(net_if.mInterfaceId);
        }
    }

    if (allocator_tbl.empty())
    {   // for local HS of xNX, we come here if interface is not specified
        auto &allocator = allocator_tbl[FDB_IP_ALL_INTERFACE];
        allocator.setInterfaceIp(FDB_IP_ALL_INTERFACE);
        allocator.setExportableLevel(FDB_EXPORTABLE_DOMAIN);
        allocator.setInterfaceId(FDB_INVALID_ID);
    }
}

void CNameServer::createTCPAllocator()
{
    createTCPAllocator(mTCPAllocators);
}

IAddressAllocator *CNameServer::getTCPAllocator(tTCPAllocatorTbl &allocator_tbl,
                                                const std::string &ip_address)
{
#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
    // For windows, TCP is also used for local communication
    if (!ip_address.compare(FDB_LOCAL_HOST))
    {
        return &mLocalAllocator;
    }
#endif
    auto it = allocator_tbl.find(ip_address);
    if (it == allocator_tbl.end())
    {
        return 0;
    }
    else
    {
        return &it->second;
    }
}

IAddressAllocator *CNameServer::getTCPAllocator(const std::string &ip_address)
{
    return getTCPAllocator(mTCPAllocators, ip_address);
}

void CNameServer::allocateTCPAddress(CTCPAddressAllocator &allocator, CSvcRegistryEntry &addr_tbl,
                                     tSocketAddrTbl &sckt_addr_tbl, bool secure)
{
    auto network_list = mServerSecruity.getNetworkList(addr_tbl.mSvcName.c_str());
    if (!network_list || network_list->empty())
    {
        sckt_addr_tbl.resize(sckt_addr_tbl.size() + 1);
        auto &sckt_addr = sckt_addr_tbl.back();
        if (!allocateAddress(allocator, addr_tbl, sckt_addr, secure))
        {
            sckt_addr_tbl.pop_back();
        }
    }
    else
    {
        for (auto it = network_list->begin(); it != network_list->end(); ++it)
        {
            if (it->mType != FDB_SOCKET_TCP)
            {
                continue;
            }
            if (it->mSecure != secure)
            {
                continue;
            }
            if (allocator.interfaceIp() != it->mAddr)
            {
                continue;
            }
            sckt_addr_tbl.resize(sckt_addr_tbl.size() + 1);
            auto &sckt_addr = sckt_addr_tbl.back();
            if (allocator.allocateKnownAddr(sckt_addr, addr_tbl.mSvcType, secure))
            {
                if (findAddress(sckt_addr.mSocketAddr.mType, sckt_addr.mSocketAddr.mUrl.c_str()))
                {
                    sckt_addr_tbl.pop_back();
                    LOG_E("NameServer: Unable to allocate address for %d!\n", addr_tbl.mSvcType);
                }
                continue;
            }
            if (it->mPort == FDB_INET_PORT_AUTO)
            {
                if (!allocateAddress(allocator, addr_tbl, sckt_addr, secure))
                {
                    sckt_addr_tbl.pop_back();
                    LOG_E("NameServer: Unable to allocate address for %s!\n", it->mUrl.c_str());
                }
            }
            else
            {
                sckt_addr.mSocketAddr = *it;
                if (findAddress(it->mType, it->mUrl.c_str()))
                {
                    LOG_E("NameServer: Unable to allocate address for %s!\n", it->mUrl.c_str());
                    sckt_addr_tbl.pop_back();
                }
            }
        }
    }
}

void CNameServer::allocateTCPAddress(CTCPAddressAllocator &allocator, CSvcRegistryEntry &addr_tbl,
                                     tSocketAddrTbl &sckt_addr_tbl)
{
    if (addr_tbl.mAllowTcpNormal)
    {
        allocateTCPAddress(allocator, addr_tbl, sckt_addr_tbl, false);
    }
    if (addr_tbl.mAllowTcpSecure)
    {
        allocateTCPAddress(allocator, addr_tbl, sckt_addr_tbl, true);
    }
}

void CNameServer::allocateTCPAddress(tTCPAllocatorTbl &allocator_tbl, CSvcRegistryEntry &addr_tbl,
                                     tSocketAddrTbl &sckt_addr_tbl)
{
     for (auto it = allocator_tbl.begin(); it != allocator_tbl.end(); ++it)
     {
         if (addr_tbl.mExportableLevel >= it->second.exportableLevel())
         {
             allocateTCPAddress(it->second, addr_tbl, sckt_addr_tbl);
         }
     }
}

void CNameServer::allocateTCPAddress(CSvcRegistryEntry &addr_tbl, tSocketAddrTbl &sckt_addr_tbl)
{
    if (addr_tbl.mSvcType == FDB_SVC_HOST_SERVER)
    {   // allocate allocator table to avoid changing mTCPAllocators;
        // mTCPAllocators will be created at onHostOnline, i.e., upon
        // connection of the first host server.
        tTCPAllocatorTbl allocator_tbl;
        createTCPAllocator(allocator_tbl);
        allocateTCPAddress(allocator_tbl, addr_tbl, sckt_addr_tbl);
        return;
    }

    allocateTCPAddress(mTCPAllocators, addr_tbl, sckt_addr_tbl);
}

void CNameServer::allocateIPCAddress(CSvcRegistryEntry &addr_tbl, tSocketAddrTbl &sckt_addr_tbl)
{
#if !defined(__WIN32__) && !defined(CONFIG_FORCE_LOCALHOST)
    sckt_addr_tbl.resize(sckt_addr_tbl.size() + 1);
    if (!allocateAddress(mIPCAllocator, addr_tbl, sckt_addr_tbl.back(), false))
    {
        sckt_addr_tbl.pop_back();
    }
#endif
}

EFdbSocketType CNameServer::getSocketType(FdbSessionId_t sid)
{
    auto session = getSession(sid);
    if (session)
    {
        CFdbSessionInfo sinfo;
        session->getSessionInfo(sinfo);
        return sinfo.mContainerSocket.mAddress->mType;
    }
    else
    {
        return FDB_SOCKET_TCP;
    }
}

void CNameServer::onOffline(const CFdbOnlineInfo &info)
{
    while (1)
    {
        bool rescan = false;
        for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
        {
            for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
            {
                auto &reg_entry = it_id->second;
                if (reg_entry.mSid == info.mSid)
                {
                    rescan = removeService(reg_entry.mSvcName.c_str(), reg_entry.mInstanceId);
                    if (rescan)
                    {
                        break;
                    }
                }
            }
            if (rescan)
            {
                break;
            }
        }
        if (!rescan)
        {
            break;
        }
    }
}

void CNameServer::onBark(CFdbSession * session)
{
    CFdbMsgProcessList process_list;

    auto process = process_list.add_process_list();
    process->set_client_name(session->senderName().c_str());
    process->set_pid((uint32_t)(session->pid()));

    CFdbParcelableBuilder builder(process_list);
    broadcast(NTF_WATCHDOG, builder);
}

bool CNameServer::bindNsAddress(tAddressDescTbl &addr_tbl)
{
    bool success = true;
    for (auto it = addr_tbl.begin(); it != addr_tbl.end();)
    {
        auto the_it = it;
        ++it;
        if (the_it->mStatus == CFdbAddressDesc::ADDR_BOUND)
        {
            continue;
        }
        auto url = the_it->mAddress.mUrl.c_str();
        auto sk = doBind(url);
        if (sk)
        {
            the_it->mStatus = CFdbAddressDesc::ADDR_BOUND;
            CFdbSocketInfo socket_info;
            if (sk->getUDPSocketInfo(socket_info) && FDB_VALID_PORT(socket_info.mAddress->mPort))
            {
                the_it->mUDPPort = socket_info.mAddress->mPort;
            }
            else
            {
                the_it->mUDPPort = FDB_INET_PORT_INVALID;
            }
        }
        else
        {
            LOG_E("NameServer: Fail to bind address: %s!\n", url);
            addr_tbl.erase(the_it);
            success = false;
        }
        
    }
    return success;
}

void CNameServer::importNetInterface(const char *net_interface)
{
    if (!net_interface || (net_interface[0] == '\0'))
    {
        return;
    }
    mNetInterfaces.resize(mNetInterfaces.size() + 1);
    auto &net_if = mNetInterfaces.back();
    std::string if_str = net_interface;
    auto pos_level = if_str.find("+");
    int32_t level = FDB_EXPORTABLE_DOMAIN;
    int32_t if_id = FDB_INVALID_ID;
    if (pos_level == std::string::npos)
    {
        net_if.mAddress = if_str;
    }
    else
    {
        auto property = if_str.substr(pos_level + 1);
        auto pos_id = property.find("+");
        if (pos_id == std::string::npos)
        {
            try
            {
                level = std::stoi(property);
            }
            catch (...)
            {
            }
        }
        else
        {
            try
            {
                level = std::stoi(property.substr(0, pos_id));
            }
            catch (...)
            {
            }
            try
            {
                if_id = std::stoi(property.substr(pos_id + 1));
            }
            catch (...)
            {
            }
        }
        net_if.mAddress = if_str.substr(0, pos_level);
    }
    net_if.mExportableLevel = level;
    net_if.mInterfaceId = if_id;
    net_if.isIfName = !CBaseSocketFactory::isValidIpAddr(net_if.mAddress.c_str());
}

bool CNameServer::online(const CNameServer::COnlineParams &params)
{
    if (params.mInterfaceArray && params.mNumInterfaces)
    {
        for (uint32_t i = 0; i < params.mNumInterfaces; ++i)
        {
            importNetInterface(params.mInterfaceArray[i]);
        }
    }

    if (params.mForceBindAddress)
    {
        createTCPAllocator();
    }

    auto &reg_entry = mRegistryTbl[nsName()][FDB_DEFAULT_INSTANCE];
    CFdbToken::allocateToken(reg_entry.mTokens);
    reg_entry.setSvcName(nsName().c_str());

    reg_entry.mSid = FDB_INVALID_ID;
    reg_entry.mEndpointName = name();
    reg_entry.mAllowTcpSecure = TCPSecureEnabled() ? true : false;
    reg_entry.mAllowTcpNormal = true;
    reg_entry.mExportableLevel = exportableLevel();

    addServiceAddress(reg_entry, false, 0);

    if (!bindNsAddress(reg_entry.mAddrTbl))
    {
        return false;
    }

    if (params.mHostServerUrlArray && params.mNumHostServerUrl)
    {
        for (uint32_t i = 0; i < params.mNumHostServerUrl; ++i)
        {
            connectToHostServer(params.mHostServerUrlArray[i], false);
        }
    }

    if (params.mStaticHostUrlArray && params.mNumStaticHostUrl)
    {
        for (uint32_t i = 0; i < params.mNumStaticHostUrl; ++i)
        {
            const char *url = params.mStaticHostUrlArray[i];
            CFdbSocketAddr addr;
            if (CBaseSocketFactory::parseUrl(url, addr))
            {
                if (addr.mType == FDB_SOCKET_TCP)
                {
                    auto proxy = new CInterNameProxy(&mStaticNameProxy,
                                                     addr.mAddr.c_str(),
                                                     url, "static_host");
                    proxy->enableReconnect(true);
                    mStaticNameProxy.addNameProxy(proxy, addr.mAddr.c_str());
                    proxy->connectToNameServer();
                }
            }
            else
            {
                LOG_E("CNameServer: Bad static host url: %s\n", url);
            }
        }
    }

    return true;
}

void CNameServer::connectToHostServer(const char *hs_url, bool is_local)
{
    if (hs_url && (hs_url[0] != '\0'))
    {
        CIntraHostProxy *proxy;
        auto proxy_it = mHostProxyTbl.find(hs_url);
        if (proxy_it == mHostProxyTbl.end())
        {
            proxy = new CIntraHostProxy(this);
            mHostProxyTbl[hs_url] = proxy;
            proxy->setHostUrl(hs_url);
            proxy->local(is_local);
        }
        else
        {
            proxy = proxy_it->second;
        }

        proxy->connectToHostServer();
    }
}

int32_t CNameServer::getSecurityLevel(CFdbSession *session, const char *svc_name)
{
    CFdbSessionInfo session_info;
    session->getSessionInfo(session_info);
    return mServerSecruity.getSecurityLevel(svc_name, session_info.mCred->uid);
}

bool CNameServer::isLocalService(CFdbSvcAddress &exported_addr)
{
    for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
    {
        for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
        {
            auto &reg_entry = it_id->second;
            if (reg_entry.mSvcName == exported_addr.mSvcName)
            {
                for (auto addr_it = reg_entry.mAddrTbl.begin(); addr_it != reg_entry.mAddrTbl.end(); ++addr_it)
                {
                    auto &svc_addr = *addr_it;
                    for (auto sock_it = exported_addr.mAddrTbl.begin(); sock_it != exported_addr.mAddrTbl.end(); ++sock_it)
                    {   // if any address in list matches, the two services are regarded as the same
                        if (svc_addr.mAddress.mUrl == sock_it->mAddress.mUrl)
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void CNameServer::getHostTbl(FdbMsgHostAddressList &host_tbl, CFdbSession *session)
{
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->getHostTbl(host_tbl);
    }
    mStaticNameProxy.getHostTbl(host_tbl);

    auto addr = host_tbl.add_address_list();
    std::string host_ip;
    if (!session->hostIp(host_ip))
    {
        host_ip = FDB_IP_ALL_INTERFACE;
    }

    addr->set_ip_address(host_ip);
    addr->set_host_name(mHostName);
    populateNsAddrList(*addr);
}

bool CNameServer::sameSubnet(const char *ip_addr)
{
    for (auto it = mTCPAllocators.begin(); it != mTCPAllocators.end(); ++it)
    {
        return CBaseSocketFactory::sameSubnet(ip_addr, it->second.interfaceIp().c_str(),
                                              it->second.mask().c_str());
    }
    return false;
}

void CNameServer::recallServiceListener(FdbMsgCode_t instance_id, const char *service_name,
                                        SubscribeType subscribe_type)
{
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto proxy = it->second;
        proxy->recallServiceListener(instance_id, service_name, subscribe_type);
    }
    mStaticNameProxy.recallServiceListener(instance_id, service_name, subscribe_type);
}

void CNameServer::deleteNameProxy(CInterNameProxy *proxy)
{
    for (auto it = mHostProxyTbl.begin(); it != mHostProxyTbl.end(); ++it)
    {
        auto p = it->second;
        p->deleteNameProxy(proxy);
    }
}

void CNameServer::forwardServiceSubscription(CInterNameProxy *proxy)
{
    tFdbSubscribeMsgTbl registered_service_tbl;
    mIntraNormalPublisher.getSubscribeTable(registered_service_tbl);
    for (auto it_inst = registered_service_tbl.begin(); it_inst != registered_service_tbl.end(); ++it_inst)
    {
        for (auto it_name = it_inst->second.begin(); it_name != it_inst->second.end(); ++it_name)
        {
            proxy->addServiceListener(it_inst->first, it_name->c_str());
        }
    }
    tFdbSubscribeMsgTbl monitored_service_tbl;
    mIntraMonitorPublisher.getSubscribeTable(registered_service_tbl);
    for (auto it_inst = monitored_service_tbl.begin(); it_inst != monitored_service_tbl.end(); ++it_inst)
    {
        for (auto it_name = it_inst->second.begin(); it_name != it_inst->second.end(); ++it_name)
        {
            proxy->addServiceMonitorListener(it_inst->first, it_name->c_str());
        }
    }
}

bool CNameServer::findService(tRegistryTbl::iterator it_name, FdbInstanceId_t instance_id,
                              tRegistryMatchTbl *reg_tbl)
{
    bool found = false;
    if (fdbIsGroup(instance_id))
    {
        if (fdbEventInGroup(instance_id, FDB_EVENT_ALL_GROUPS))
        {
            if (!it_name->second.empty())
            {
                found = true;
            }
            if (reg_tbl)
            {
                for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
                {
                    reg_tbl->push_back(&it_id->second);
                }
            }
        }
        else
        {
            for (auto it_id = it_name->second.begin(); it_id != it_name->second.end(); ++it_id)
            {
                if (fdbSameGroup(instance_id, it_id->first))
                {
                    found = true;
                    if (reg_tbl)
                    {
                        reg_tbl->push_back(&it_id->second);
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }
    }
    else
    {
        auto it_id = it_name->second.find(instance_id);
        if (it_id != it_name->second.end())
        {
            found = true;
            if (reg_tbl)
            {
                reg_tbl->push_back(&it_id->second);
            }
        }
    }

    return found;
}

bool CNameServer::findService(const char *svc_name, FdbInstanceId_t instance_id,
                              tRegistryMatchTbl *reg_tbl)
{
    bool found = false;
    if (!svc_name || svc_name[0] == '\0')
    {
        for (auto it_name = mRegistryTbl.begin(); it_name != mRegistryTbl.end(); ++it_name)
        {
            if (findService(it_name, instance_id, reg_tbl))
            {
                found = true;
            }
        }
    }
    else
    {
        auto it_name = mRegistryTbl.find(svc_name);
        if (it_name != mRegistryTbl.end())
        {
            found = findService(it_name, instance_id, reg_tbl);
        }
    }

    return found;
}

bool CNameServer::eraseService(const char *svc_name, FdbInstanceId_t instance_id)
{
    bool erased = false;
    auto it_name = mRegistryTbl.find(svc_name);
    if (it_name != mRegistryTbl.end())
    {
        auto it_id = it_name->second.find(instance_id);
        if (it_id != it_name->second.end())
        {
            it_name->second.erase(it_id);
            erased = true;
        }
        if (it_name->second.empty())
        {
            mRegistryTbl.erase(it_name);
        }
    }
    return erased;
}

CNameServer::ConnectAddrPublisher *CNameServer::getPublisher(SubscribeType subscribe_type)
{
    static CNameServer::ConnectAddrPublisher *publishers[] = {&mIntraNormalPublisher,
                                                              &mIntraMonitorPublisher,
                                                              &mInterNormalPublisher,
                                                              &mInterMonitorPublisher};
    return publishers[subscribe_type - INTRA_NORMAL];
}

void CNameServer::getSvcSubscribeTable(FdbMsgCode_t instance_id, const char *svc_name,
                                       tSubscribedSessionSets &session_tbl, SubscribeType type)
{
    getPublisher(type)->getSubscribeTable(instance_id, svc_name, session_tbl);
}

void CNameServer::broadcastService(FdbMsgCode_t instance_id, IFdbMsgBuilder &data, const char *svc_name,
                                   SubscribeType type)
{
    getPublisher(type)->broadcast(instance_id, data, svc_name);
}

#if 0
void CNameServer::onHostIpObtained(std::string &ip_addr)
{
    if (ip_addr.empty())
    {
        return;
    }

    for (tRegistryTbl::iterator svc_it = mRegistryTbl.begin();
        svc_it != mRegistryTbl.end(); ++svc_it)
    {
        const std::string &svc_name = svc_it->first;
        CSvcRegistryEntry &addr_tbl = svc_it->second;
        FdbMsgAddressList broadcast_addr_list;

        for (tAddressDescTbl::iterator addr_it = addr_tbl.mAddrTbl.begin();
            addr_it != addr_tbl.mAddrTbl.end(); ++addr_it)
        {
            CFdbAddressDesc *desc = *addr_it;
            if (ip_addr == desc->mAddress.mAddr)
            {
                broadcast_addr_list.add_address_list(desc->mAddress.mUrl);
            }
        }

        if (broadcast_addr_list.address_list().empty())
        {
            addServiceAddress(svc_name, FDB_INVALID_ID, FDB_SOCKET_TCP, &broadcast_addr_list);
        }
        else
        {
            broadcast_addr_list.set_service_name(svc_name);
            broadcast_addr_list.set_host_name(mHostName);
            broadcast_addr_list.set_is_local(true);
        }
        
        if (svc_name != name())
        {
            if (!broadcast_addr_list.address_list().empty())
            {
                broadcast(NTF_MORE_ADDRESS, broadcast_addr_list, svc_name.c_str());
            }
        }
        else
        {
            bindNsAddress(addr_tbl.mAddrTbl);
        }
    }
    mHostProxy->connectToHostServer();
}
#endif
}
}
