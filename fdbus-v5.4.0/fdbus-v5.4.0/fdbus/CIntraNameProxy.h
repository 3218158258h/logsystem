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

#ifndef _CINTRANAMEPROXY_H_
#define _CINTRANAMEPROXY_H_

#include <vector>
#include <string>
#include <fdbus/CNotificationCenter.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CMethodJob.h>
#include <fdbus/CBaseNameProxy.h>

namespace ipc {
namespace fdbus {
class CFdbBaseContext;

class CIntraNameProxy : public CBaseNameProxy
{
public:
    struct CHostNameReady
    {
        CHostNameReady(std::string &host_name)
            : mHostName(host_name)
        {
        }
        std::string &mHostName;
    };
    CIntraNameProxy();
    void listenOnService(CBaseEndpoint *endpoint);
    void listenOnService(FdbInstanceId_t instance_id, const char *svc_name, FdbContextId_t ctx_id,
                         FdbEndpointId_t ep_id);
    void removeServiceListener(FdbInstanceId_t instance_id, const char *svc_name);
    void addAddressListener(FdbInstanceId_t instance_id, const char *svc_name);
    void removeAddressListener(FdbInstanceId_t instance_id, const char *svc_name);
    void registerService(CBaseEndpoint *endpoint);
    void unregisterService(CBaseEndpoint *endpoint);
    std::string &hostName()
    {
        return mHostName;
    }
    void registerHostNameReadyNotify(CBaseNotification<CHostNameReady> *notification);
    void registerNsWatchdogListener(tNsWatchdogListenerFn &watchdog_listener);
    void monitorService(CBaseNotification<FdbMsgAddressList> *notification,
                        const char *svc_name,
                        FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE);
    void removeServiceMonitor(CBaseNotification<FdbMsgAddressList> *notification,
                              const char *svc_name,
                              FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE);
    static void doConnectToAddress(CBaseClient *client,
                                   FdbMsgAddressList &msg_addr_list,
                                   bool &udp_failure,
                                   bool &udp_success,
                                   bool &tcp_failure);
protected:
    void onReply(CBaseJob::Ptr &msg_ref);
    void onBroadcast(CBaseJob::Ptr &msg_ref);
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);
    void validateUrl(FdbMsgAddressList &msg_addr_list, CFdbSession *session);
    
private:
    class ConnectAddrSubscriber : public CFdbBaseObject
    {
    public:
        ConnectAddrSubscriber(CIntraNameProxy *ns_proxy, SubscribeType type, const char *name);
        void connectWithProxy();
    protected:
        void onBroadcast(CBaseJob::Ptr &msg_ref);
    private:
        CIntraNameProxy *mNsProxy;
        SubscribeType mType;
    };

    class BindAddrSubscriber : public CFdbBaseObject
    {
    public:
        BindAddrSubscriber(CIntraNameProxy *ns_proxy, const char *name);
        void connectWithProxy();
    protected:
        void onBroadcast(CBaseJob::Ptr &msg_ref);
    private:
        CIntraNameProxy *mNsProxy;
    };

    std::string mHostName;
    CBaseNotificationCenter<CHostNameReady> mHostNameNtfCenter;
    tNsWatchdogListenerFn mNsWatchdogListener;

    ConnectAddrSubscriber mIntraNormalSubscriber;
    ConnectAddrSubscriber mIntraMonitorSubscriber;
    CBaseNotificationCenter<FdbMsgAddressList> mSvcListNtfCenter;
    BindAddrSubscriber mBindAddrSubscriber;

    void doConnectToServer(CFdbBaseContext *context, FdbEndpointId_t ep_id,
                           FdbMsgAddressList &msg_addr_list, bool is_init_response);
    void doBindAddress(CFdbBaseContext *context, FdbEndpointId_t ep_id,
                       FdbMsgAddressList &msg_addr_list, bool force_rebind);
    void doRegisterNsWatchdogListener(tNsWatchdogListenerFn &watchdog_listener);
    void connectToServer(CFdbMessage *msg, FdbContextId_t ctx_id, FdbEndpointId_t ep_id);
    void bindAddress(CFdbMessage *msg, FdbContextId_t ctx_id, FdbEndpointId_t ep_id);
    void queryServiceAddress();
    void callConnectToServer(CMethodJob<CIntraNameProxy> *job, CBaseJob::Ptr &ref);
    void callBindAddress(CMethodJob<CIntraNameProxy> *job, CBaseJob::Ptr &ref);
    void callQueryServiceAddress(CMethodJob<CIntraNameProxy> *job, CBaseJob::Ptr &ref);
    void onServiceBroadcast(CBaseJob::Ptr &msg_ref);
    void notifySvcOnlineMonitor(CFdbMessage *msg);

    friend class CRegisterWatchdogJob;
    friend class CConnectToServerJob;
    friend class CBindAddressJob;
    friend class CQueryServiceAddressJob;
    friend class ConnectAddrSubscriber;
    friend class BindAddrSubscriber;
};
}
}
#endif
