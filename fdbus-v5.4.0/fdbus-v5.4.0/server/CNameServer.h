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

#ifndef _CNAMESERVER_H_
#define _CNAMESERVER_H_

#include <list>
#include <map>
#include <string>
#include <vector>
#include <fdbus/CBaseServer.h>
#include <fdbus/CSocketImp.h>
#include <security/CServerSecurityConfig.h>
#include <fdbus/CFdbMsgDispatcher.h>
#include "CAddressAllocator.h"
#include "CSvcAddrUtils.h"
#include "CNameProxyContainer.h"
#include <fdbus/CBaseNameProxy.h>

namespace ipc {
namespace fdbus {
class FdbMsgServiceTable;
class FdbMsgAddressList;
class FdbMsgServiceInfo;
class FdbMsgAddressItem;
class FdbMsgHostAddress;
class FdbMsgExportableSvcAddress;
class FdbMsgHostAddressList;
class FdbMsgAllExportableService;

class CFdbMessage;
class CIntraHostProxy;
class CFdbSession;

class CNameServer : public CBaseServer
{
private:
#define INVALID_ALLOC_ID        (~0)
    struct CFdbAddressDesc : public CFdbSocketAddrInfo
    {
        enum eAddressStatus
        {
            ADDR_FREE,
            ADDR_PENDING,
            ADDR_BOUND,
        };
        CFdbAddressDesc()
            : CFdbSocketAddrInfo()
            , mStatus(ADDR_FREE)
            , mRebindCount(0)
        {
        }
        eAddressStatus mStatus;
        // Actually it doesn't make sense to have UDP port for server
        int32_t mRebindCount;
    };

public:
    CNameServer(const char *host_name,
                int32_t self_exportable_level,
                int32_t max_domain_exportable_level,
                int32_t min_upstream_exportable_level);
    ~CNameServer();
    struct COnlineParams
    {
        char **mHostServerUrlArray;
        uint32_t mNumHostServerUrl;
        char **mInterfaceArray;
        uint32_t mNumInterfaces;
        char **mStaticHostUrlArray;
        uint32_t mNumStaticHostUrl;
        bool mForceBindAddress;
    };
    bool online(const COnlineParams &params);
    void populateServerTable(CFdbSession *session, FdbMsgServiceTable &svc_tbl, bool is_local);

    void notifyRemoteNameServerDrop(const char *host_name);
    void onHostOnline(bool online, CIntraHostProxy *host_proxy);
    void populateNsAddrList(FdbMsgHostAddress &host_address);
    void populateExportAddrList(FdbMsgExportableSvcAddress &export_svc_addr_list);
    bool isLocalService(CFdbSvcAddress &exported_addr);
    std::string &hostName() /* local */
    {
        return mHostName;
    }
    void getHostTbl(FdbMsgHostAddressList &host_tbl, CFdbSession *session);
    void finalizeServiceQuery(FdbMsgServiceTable *svc_tbl, CFdbMessage *msg);
    void finalizeExportableServiceQuery(FdbMsgAllExportableService *svc_tbl,
                                        CFdbMessage *msg);
    int32_t maxDomainExportableLevel() const
    {
        return mMaxDomainExportableLevel;
    }
    bool sameSubnet(const char *ip_addr);
    CNameProxyContainer &getStaticNameProxyContainer()
    {
        return mStaticNameProxy;
    }
    void recallServiceListener(FdbMsgCode_t instance_id, const char *service_name,
                               SubscribeType subscribe_type);
    void deleteNameProxy(CInterNameProxy *proxy);
    void forwardServiceSubscription(CInterNameProxy *proxy);
    void getSvcSubscribeTable(FdbMsgCode_t instance_id, const char *svc_name,
                              tSubscribedSessionSets &session_tbl, SubscribeType type);
    void broadcastService(FdbMsgCode_t instance_id, IFdbMsgBuilder &data, const char *svc_name,
                          SubscribeType type);
protected:
    void onSubscribe(CBaseJob::Ptr &msg_ref);
    void onInvoke(CBaseJob::Ptr &msg_ref);
    void onOffline(const CFdbOnlineInfo &info);
    void onBark(CFdbSession * session);
private:
    class ConnectAddrPublisher : public CFdbBaseObject
    {
    public:
        ConnectAddrPublisher(CNameServer *ns, SubscribeType type, const char *name);
        void bindToNs();
    protected:
        void onSubscribe(CBaseJob::Ptr &msg_ref);
    private:
        CNameServer *mNameServer;
        SubscribeType mType;
    };
    class BindAddrPublisher : public CFdbBaseObject
    {
    public:
        BindAddrPublisher(CNameServer *ns, const char *name);
        void bindToNs();
    private:
        CNameServer *mNameServer;
    };

    typedef std::list<CFdbAddressDesc> tAddressDescTbl;
    typedef std::map<std::string, CIntraHostProxy *> tHostProxyTbl;
    struct CSvcRegistryEntry
    {
        CSvcRegistryEntry()
            : mSid(FDB_INVALID_ID)
            , mAllowTcpSecure(false)
            , mAllowTcpNormal(true)
            , mExportableLevel(FDB_EXPORTABLE_NODE_INTERNAL)
            , mSvcType(FDB_SVC_UNKNOWN)
            , mInstanceId(FDB_DEFAULT_INSTANCE)
        {}
        void setSvcName(const char *svc_name)
        {
            mSvcName = svc_name;
            mSvcType = IAddressAllocator::getSvcType(svc_name);
        }
        FdbSessionId_t mSid;
        tAddressDescTbl mAddrTbl;
        CFdbToken::tTokenList mTokens;
        bool mAllowTcpSecure;
        bool mAllowTcpNormal;
        int32_t mExportableLevel;
        std::string mEndpointName;
        std::string mSvcName;
        FdbServerType mSvcType;
        FdbInstanceId_t mInstanceId;
    };
    typedef std::map<FdbInstanceId_t, CSvcRegistryEntry> tSvcRegistryTbl;
    typedef std::map<std::string, tSvcRegistryTbl> tRegistryTbl;
    typedef std::map<std::string, CTCPAddressAllocator> tTCPAllocatorTbl;
    typedef std::map<std::string, CUDPPortAllocator> tUDPAllocatorTbl;
    typedef std::vector<CAllocatedAddress> tSocketAddrTbl;
    struct CNetInterface
    {
        std::string mAddress;
        int32_t mExportableLevel;
        int32_t mInterfaceId;
        bool isIfName;
    };
    typedef std::list<CNetInterface> tInterfaceTbl;
    typedef std::vector<CSvcRegistryEntry *> tRegistryMatchTbl;

    tRegistryTbl mRegistryTbl;
    CFdbMessageHandle<CNameServer> mMsgHdl;
    CFdbSubscribeHandle<CNameServer> mSubscribeHdl;

#if defined(__WIN32__) || defined(CONFIG_FORCE_LOCALHOST)
    CTCPAddressAllocator mLocalAllocator; // local host address(lo) allocator
#else
    CIPCAddressAllocator mIPCAllocator; // UDS address allocator
#endif
    tTCPAllocatorTbl mTCPAllocators; // TCP (other than lo for windows) address allocator
    tUDPAllocatorTbl mUDPAllocators; // UDP port allocator
    CServerSecurityConfig mServerSecruity;
    tInterfaceTbl mNetInterfaces;
    std::string mHostName;
    tHostProxyTbl mHostProxyTbl;
    // The maximum level to be exported inside the domain
    int32_t mMaxDomainExportableLevel;
    // The minimum level to be exported outside the domain (to upstream host server)
    int32_t mMinUpstreamExportableLevel;
    std::string mDomainName;

    CNameProxyContainer mStaticNameProxy;

    ConnectAddrPublisher mIntraNormalPublisher;
    ConnectAddrPublisher mIntraMonitorPublisher;
    ConnectAddrPublisher mInterNormalPublisher;
    ConnectAddrPublisher mInterMonitorPublisher;

    BindAddrPublisher mBindAddrPublisher;

    uint32_t mInstanceIdAllocator;

    void populateAddrList(const tAddressDescTbl &addr_tbl, FdbMsgAddressList &list,
                          EFdbSocketType type, int32_t max_exported_level);

    void onAllocServiceAddressReq(CBaseJob::Ptr &msg_ref);
    void onRegisterServiceReq(CBaseJob::Ptr &msg_ref);
    void onUnegisterServiceReq(CBaseJob::Ptr &msg_ref);
    void onQueryServiceReq(CBaseJob::Ptr &msg_ref);
    void onQueryServiceInterMachineReq(CBaseJob::Ptr &msg_ref);
    void onQueryExportableServiceReq(CBaseJob::Ptr &msg_ref);
    void onQueryHostReq(CBaseJob::Ptr &msg_ref);
    
    void onServiceOnlineReg(const char *svc_name, FdbInstanceId_t instance_id, CBaseJob::Ptr &msg_ref,
                            SubscribeType subscribe_type);
    void onHostOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item);
    void onHostInfoReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item);
    void onWatchdogReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item);

    CFdbAddressDesc *findAddress(EFdbSocketType type, const char *url);
    void createTCPAllocator();
    void createTCPAllocator(tTCPAllocatorTbl &allocator_tbl);
    bool allocateAddress(IAddressAllocator &allocator, const CSvcRegistryEntry &addr_tbl,
                         CAllocatedAddress &allocated_addr, bool secure);
    
    IAddressAllocator *getTCPAllocator(const std::string &ip_address);
    IAddressAllocator *getTCPAllocator(tTCPAllocatorTbl &allocator_tbl, const std::string &ip_address);
    void allocateTCPAddress(CTCPAddressAllocator &allocator, CSvcRegistryEntry &addr_tbl,
                            tSocketAddrTbl &sckt_addr_tbl, bool secure);
    void allocateTCPAddress(CTCPAddressAllocator &allocator,  CSvcRegistryEntry &addr_tbl,
                            tSocketAddrTbl &sckt_addr_tbl);
    void allocateTCPAddress(CSvcRegistryEntry &addr_tbl, tSocketAddrTbl &sckt_addr_tbl);
    void allocateTCPAddress(tTCPAllocatorTbl &allocator_tbl, CSvcRegistryEntry &addr_tbl,
                            tSocketAddrTbl &sckt_addr_tbl);
    void allocateIPCAddress(CSvcRegistryEntry &addr_tbl, tSocketAddrTbl &sckt_addr_tbl);

    EFdbSocketType getSocketType(FdbSessionId_t sid);
    bool removeService(const char *svc_name, FdbInstanceId_t instance_id);
    void connectToHostServer(const char *hs_url, bool is_local);
    bool addressRegistered(const tAddressDescTbl &addr_list, CFdbSocketAddr &sckt_addr);
    void addOneServiceAddress(CSvcRegistryEntry &addr_tbl,
                              bool inet_address,
                              FdbMsgAddressList *msg_addr_list);
    bool addServiceAddress(CSvcRegistryEntry &addr_tbl,
                           bool inet_only,
                           FdbMsgAddressList *msg_addr_list);
    void setHostInfo(CFdbSession *session, FdbMsgServiceInfo *msg_svc_info, const char *svc_name);
    void broadServiceAddress(const CSvcRegistryEntry &reg_entry, CFdbMessage *msg,
                             SubscribeType subscribe_type);
    bool bindNsAddress(tAddressDescTbl &addr_tbl);
    bool rebindToAddress(CFdbAddressDesc *addr_desc, const CSvcRegistryEntry &addr_tbl);
    void populateTokensRemote(const CFdbToken::tTokenList &tokens,
                               FdbMsgAddressList &addr_list,
                               CFdbSession *session);
    void broadcastSvcAddrRemote(const CFdbToken::tTokenList &tokens,
                                 FdbMsgAddressList &addr_list,
                                 CFdbMessage *msg);
    void broadcastSvcAddrRemote(const CFdbToken::tTokenList &tokens,
                                 FdbMsgAddressList &addr_list,
                                 CFdbSession *session);
    void broadcastSvcAddrRemote(const CFdbToken::tTokenList &tokens,
                                 FdbMsgAddressList &addr_list);
    void populateTokensLocal(const CFdbToken::tTokenList &tokens,
                              FdbMsgAddressList &addr_list,
                              CFdbSession *session);
    void broadcastSvcAddrLocal(const CFdbToken::tTokenList &tokens,
                                FdbMsgAddressList &addr_list,
                                CFdbMessage *msg);
    void broadcastSvcAddrLocal(const CFdbToken::tTokenList &tokens,
                                FdbMsgAddressList &addr_list,
                                CFdbSession *session);
    void broadcastSvcAddrLocal(const CFdbToken::tTokenList &tokens,
                                FdbMsgAddressList &addr_list);
    bool hasPendingAddress(const CSvcRegistryEntry &entry);

    int32_t getSecurityLevel(CFdbSession *session, const char *svc_name);

    bool checkExportable(const CFdbAddressDesc &addr_desc, int32_t max_exported_level);
    void populateAddressItem(const CFdbAddressDesc &addr_desc, FdbMsgAddressItem *item);
    void prepareAddress(const CFdbAddressDesc &addr_desc, FdbMsgAddressList &list,
                        int32_t max_exported_level);
    void prepareAddress(const CFdbAddressDesc &addr_desc, FdbMsgHostAddress &host_address,
                        int32_t max_exported_level);
    bool allocateUDPPort(const char *ip_address, int32_t &port);
    void allocateUDPPortForClients(FdbMsgAddressList &addr_list);
    CFdbAddressDesc *findUDPPort(const char *ip_address, int32_t port);
    void setServiceInfo(FdbMsgAddressList &addr_list, const char *svc_name,
                        const char *endpoint_name, bool is_local, CBASE_tProcId pid,
                        FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE);
    void setServiceInfo(FdbMsgAddressList &addr_list, const CSvcRegistryEntry &registry,
                        bool is_local, CBASE_tProcId pid);
 
    void importNetInterface(const char *net_interface);
    void prepareAddressesToNotify(std::string &best_local_url, CSvcRegistryEntry &reg_entry,
                                  FdbMsgAddressList &local_addr_list,
                                  FdbMsgAddressList &remote_tcp_addr_list,
                                  FdbMsgAddressList &all_addr_list);
 
    bool findService(tRegistryTbl::iterator it_name,
                     FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE,
                     tRegistryMatchTbl *reg_tbl = 0);
    bool findService(const char *svc_name,
                     FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE,
                     tRegistryMatchTbl *reg_tbl = 0);
    bool eraseService(const char *svc_name, FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE);
    ConnectAddrPublisher *getPublisher(SubscribeType subscribe_type);

    friend class CInterNameProxy;
    friend class CIntraHostProxy;
    friend class ConnectAddrPublisher;
};
}
}
#endif
