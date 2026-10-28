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

#ifndef _CINTRAHOSTPROXY_H_
#define _CINTRAHOSTPROXY_H_

#include <map>
#include "CBaseHostProxy.h"
#include <utils/CNsConfig.h>
#include <fdbus/CFdbMsgDispatcher.h>
#include "CSvcAddrUtils.h"
#include "CNameProxyContainer.h"

namespace ipc {
namespace fdbus {
class CInterNameProxy;
class CNameServer;
class CFdbSession;

class FdbMsgServiceTable;
class FdbMsgHostAddressList;
class FdbMsgExportableSvcAddress;
class FdbMsgHostAddress;

class CIntraHostProxy : public CBaseHostProxy, public CNameProxyContainer
{
public:
    CIntraHostProxy(CNameServer *ns);
    ~CIntraHostProxy();

    std::string &getSelfIp()
    {
        return mSelfIp;
    }
    void postSvcTable(FdbMsgExportableSvcAddress &svc_address_tbl);
    void broadcastExternalService(bool remove,
                                  SubscribeType subscribe_type,
                                  FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE,
                                  const char *svc_name = 0,
                                  CFdbMessage *msg = 0);
    void dumpExportedSvcInfo(FdbMsgExportableSvcAddress &svc_info_tbl);
    bool sendQueryExportableServiceReq(CFdbMessage *msg);
protected:
    void onReply(CBaseJob::Ptr &msg_ref);
    void onBroadcast(CBaseJob::Ptr &msg_ref);
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);
    void isolate();
private:
    std::string mSelfIp;
    tSvcAddrDescTbl mSvcAddrTbl;

    void onHostOnlineNotify(CFdbMessage *msg);
    void onHeartbeatOk(CBaseJob::Ptr &msg_ref);
    void hostOnline(FdbMsgCode_t code);
    void hostOnline();
    void hostOffline();
    void isolate(tNameProxyTbl::iterator &it);

    bool importTokens(const FdbMsgHostAddress &host_addr,
                                        CBaseEndpoint *endpoint);
    bool getNsAddress(FdbMsgHostAddress &host_addr, CFdbSession *session,
                      std::string &ns_ip, std::string &ns_url);
    void broadcastOneExtService(CFdbSvcAddress &exported_addr, bool remove,
                                 SubscribeType subscribe_type, CFdbMessage *msg);
    void broadcastExternalService(tSvcAddrDescTbl &svc_addr_tbl,
                                  bool remove,
                                  SubscribeType subscribe_type,
                                  FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE,
                                  const char *svc_name = 0, CFdbMessage *msg = 0);
};
}
}

#endif
