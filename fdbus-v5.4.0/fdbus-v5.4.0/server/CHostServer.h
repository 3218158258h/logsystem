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

#ifndef _CHOSTSERVER_H_
#define _CHOSTSERVER_H_
#include <map>
#include <string>
#include <vector>
#include <fdbus/CBaseServer.h>
#include <fdbus/CMethodLoopTimer.h>
#include <utils/CNsConfig.h>
#include <security/CHostSecurityConfig.h>
#include <fdbus/CFdbMsgDispatcher.h>
#include "CSvcAddrUtils.h"

namespace ipc {
namespace fdbus {
class FdbMsgHostRegisterAck;
class FdbMsgHostAddress;
class FdbMsgHostAddressList;
class FdbMsgExportableSvcAddress;
class FdbMsgAddressList;
class CFdbMessage;
class CInterHostProxy;

class CHostServer : public CBaseServer
{
public:
    CHostServer(const char *domain_name,
                int32_t self_exportable_level,
                int32_t max_domain_exportable_level,
                int32_t min_upstream_exportable_level);
    ~CHostServer();
    void broadcastToLocalHost(CFdbMessage *msg = 0);
    void broadcastToDownstreamHost(CFdbMessage *msg = 0);
    void buildAddrTblForUpstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl);
    void updateUpstreamSvcTable(tSvcAddrDescTbl &svc_desc_tbl,
                                FdbMsgExportableSvcAddress &svc_address_tbl);
    void online(char **upstream_host_array, int32_t num_upstream_hosts);
protected:
    void onSubscribe(CBaseJob::Ptr &msg_ref);
    void onInvoke(CBaseJob::Ptr &msg_ref);
    void onOffline(const CFdbOnlineInfo &info);
private:
    typedef std::list<CFdbSocketAddr> tHostAddressTbl;
    struct CLocalHostInfo
    {
        std::string mHostName;
        std::string mIpAddress;
        tHostAddressTbl mAddressTbl;
        std::string mNsUrl;
        int32_t mHbCount;
        bool mReady;
        bool mAuthorized;
        CFdbToken::tTokenList mTokens;
        tSvcAddrDescTbl mSvcAddrTbl;
    };
    typedef std::map<FdbSessionId_t, CLocalHostInfo> tLocalHostTbl;
    struct CExternalDomainInfo
    {
        std::string mDomainName;
        int32_t mHbCount;
        bool mReady;
        bool mAuthorized;
        CFdbToken::tTokenList mTokens;
        tSvcAddrDescTbl mSvcAddrTbl;
    };
    typedef std::map<FdbSessionId_t, CExternalDomainInfo> tExternalDomainTbl;
    typedef std::map<std::string, CInterHostProxy*> tHostProxyTbl;
    tLocalHostTbl mLocalHostTbl;
    tExternalDomainTbl mExternalDomainTbl;
    tHostProxyTbl mHostProxyTbl;
    CFdbMessageHandle<CHostServer> mMsgHdl;
    CFdbSubscribeHandle<CHostServer> mSubscribeHdl;
    // The maximum level to be exported inside the domain
    int32_t mMaxDomainExportableLevel;
    // The minimum level to be exported outside the domain (to upstream host server)
    int32_t mMinUpstreamExportableLevel;

    void onRegisterLocalHostReq(CBaseJob::Ptr &msg_ref);
    void onRegisterExternalDomainReq(CBaseJob::Ptr &msg_ref);
    void onUnregisterLocalHostReq(CBaseJob::Ptr &msg_ref);
    void onQueryHostReq(CBaseJob::Ptr &msg_ref);
    void onHeartbeatOk(CBaseJob::Ptr &msg_ref);
    void onLocalHostReady(CBaseJob::Ptr &msg_ref);
    void onExternalHostReady(CBaseJob::Ptr &msg_ref);
    void onPostLocalSvcAddress(CBaseJob::Ptr &msg_ref);
    void onPostDomainSvcAddress(CBaseJob::Ptr &msg_ref);
    void onQueryExportableService(CBaseJob::Ptr &msg_ref);

    void onHostOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item);
    void onLocalSvcOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item);
    void onExternalSvcOnlineReg(CBaseJob::Ptr &msg_ref, const CFdbMsgSubscribeItem *sub_item);

    void broadcastSingleHost(bool online, CLocalHostInfo &info);

    class CHeartBeatTimer : public CMethodLoopTimer<CHostServer>
    {
    public:
        CHeartBeatTimer(CHostServer *proxy)
            : CMethodLoopTimer<CHostServer>(CNsConfig::getHeartBeatInterval(), true,
                                            proxy, &CHostServer::broadcastHeartBeat)
        {
        }
    };
    CHeartBeatTimer mHeartBeatTimer;
    void broadcastHeartBeat(CMethodLoopTimer<CHostServer> *timer);
    CHostSecurityConfig mHostSecurity;

    void populateTokens(const CFdbToken::tTokenList &tokens,
                        FdbMsgHostRegisterAck &list);
    void addToken(const CLocalHostInfo &this_host, const CLocalHostInfo &that_host,
                    FdbMsgHostAddress &host_addr);
    FdbMsgHostAddress *populateHostAddressList(CFdbSession *session,
                                    FdbMsgHostAddressList &addr_list, CLocalHostInfo &info);
    void updateLocalSvcTable(CLocalHostInfo &host_info, FdbMsgExportableSvcAddress &svc_address_tbl);
    void populateLocalHost(FdbMsgExportableSvcAddress &svc_address_tbl, int32_t exportable_level);
    void populateUpstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl, int32_t exportable_level);
    void populateDownstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl, int32_t exportable_level);
    void buildAddrTblForLocalHost(FdbMsgExportableSvcAddress &svc_address_tbl);
    void buildAddrTblForDownstreamHost(FdbMsgExportableSvcAddress &svc_address_tbl);
    void postToUpstreamHost();
    void updateDomainSvcTable(CExternalDomainInfo &domain_info,
                              FdbMsgExportableSvcAddress &svc_address_tbl);
    void broadcastOnLocalSvcTableChanged();
    void broadcastOnDomainSvcTableChanged();
    void broadcastOnUpstreamSvcTableChanged();
};
}
}

#endif
