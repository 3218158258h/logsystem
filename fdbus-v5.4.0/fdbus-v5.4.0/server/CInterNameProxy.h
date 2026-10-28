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

#ifndef _CINTERNAMEPROXY_H_
#define _CINTERNAMEPROXY_H_

#include <string>
#include <set>
#include <list>
#include <fdbus/CBaseNameProxy.h>
#include <utils/CNsConfig.h>

namespace ipc {
namespace fdbus {
class FdbMsgAddressList;
class CNameServer;
class CNameProxyContainer;
class CQueryServiceMsg : public CFdbMessage
{
public:
    CQueryServiceMsg(tPendingServiceReqTbl *pending_tbl,
                     FdbMsgServiceTable *svc_tbl,
                     CBaseJob::Ptr &req,
                     const std::string &host_ip,
                     CNameServer *name_server);
    ~CQueryServiceMsg();
    
    tPendingServiceReqTbl *mPendingReqTbl;
    FdbMsgServiceTable *mSvcTbl;
    CBaseJob::Ptr mReq;
    std::string mHostIp;
    CNameServer *mNameServer;
protected:
    void onAsyncError(Ptr &ref, FdbMsgStatusCode code, const char *reason);
};

class CInterNameProxy : public CBaseNameProxy
{
public:
    CInterNameProxy(CNameProxyContainer *container,
                    const char *host_ip,
                    const char *ns_url,
                    const char *host_name);
    ~CInterNameProxy();
    void addServiceListener(FdbInstanceId_t instance_id, const char *svc_name);
    void addServiceListener(FdbInstanceId_t instance_id, const char *svc_name, CBaseJob::Ptr &msg_ref);
    void removeServiceListener(FdbInstanceId_t instance_id, const char *svc_name);
    void addServiceMonitorListener(FdbInstanceId_t instance_id, const char *svc_name);
    void addServiceMonitorListener(FdbInstanceId_t instance_id, const char *svc_name,
                                   CBaseJob::Ptr &msg_ref);
    void removeServiceMonitorListener(FdbInstanceId_t instance_id, const char *svc_name);
    const std::string &getHostIp() const
    {
        return mIpAddress;
    }
    bool queryServiceTbl(CFdbMessage *msg);

    const std::string &getHostName() const
    {
        return mHostName;
    }
protected:
    void onReply(CBaseJob::Ptr &msg_ref);
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);

    CNameProxyContainer *mContainer;
    void validateUrl(FdbMsgAddressList &msg_addr_list, CFdbSession *session);
private:
    class ConnectAddrSubscriber : public CFdbBaseObject
    {
    public:
        ConnectAddrSubscriber(CInterNameProxy *ns_proxy, SubscribeType type, const char *name);
        void connectWithProxy();
    protected:
        void onBroadcast(CBaseJob::Ptr &msg_ref);
    private:
        CInterNameProxy *mNsProxy;
        SubscribeType mType;
    };
    std::string mIpAddress;
    std::string mHostName;

    ConnectAddrSubscriber mInterNormalSubscriber;
    ConnectAddrSubscriber mInterMonitorSubscriber;

    void onServiceBroadcast(CFdbMessage *msg, SubscribeType subscribe_type);

    friend class ConnectAddrSubscriber;
};
}
}
#endif
