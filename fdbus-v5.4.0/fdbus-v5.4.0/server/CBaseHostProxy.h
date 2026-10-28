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

#ifndef _CBASEHOSTPROXY_H_
#define _CBASEHOSTPROXY_H_
#include <fdbus/CBaseClient.h>
#include <fdbus/CMethodLoopTimer.h>
#include <fdbus/CFdbIfNameServer.h>
#include <utils/CNsConfig.h>
#include <fdbus/CFdbMsgDispatcher.h>

namespace ipc {
namespace fdbus {
class CBaseHostProxy : public CBaseClient
{
public:
    CBaseHostProxy(const char *host_url = 0);
    void setHostUrl(const char *url)
    {
        mHostUrl = url;
    }
    bool connectToHostServer();
    std::string &hostUrl()  /* remote */
    {
        return mHostUrl;
    }
    std::string &domainName()
    {
        return mDomainName;
    }
    void setDomainName(const char *dn)
    {
        if (dn)
        {
            mDomainName = dn;
        }
    }
    
protected:
    std::string mHostUrl;
    std::string mDomainName;

    void onBroadcast(CBaseJob::Ptr &msg_ref);
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);
    virtual void isolate() {}

private:
    class CConnectTimer : public CMethodLoopTimer<CBaseHostProxy>
    {
    public:
        enum eWorkMode
        {
            MODE_RECONNECT_TO_SERVER,
            MODE_HEARTBEAT,
            MODE_IDLE
        };
        CConnectTimer(CBaseHostProxy *proxy)
            : CMethodLoopTimer<CBaseHostProxy>(CNsConfig::getHsReconnectInterval(), false,
                                           proxy, &CBaseHostProxy::onConnectTimer)
            , mMode(MODE_IDLE)
        {
        }
        void startHeartbeatMode()
        {
            mMode = MODE_HEARTBEAT;
            enableOneShot(CNsConfig::getHeartBeatTimeout());
        }
        void startReconnectMode()
        {
            mMode = MODE_RECONNECT_TO_SERVER;
            enableOneShot(CNsConfig::getHsReconnectInterval());
        }
        eWorkMode mMode;
    };
    CConnectTimer mConnectTimer;
    CFdbMessageHandle<CBaseHostProxy> mNotifyHdl;

    void onConnectTimer(CMethodLoopTimer<CBaseHostProxy> *timer);
    void onHeartbeatOk(CBaseJob::Ptr &msg_ref);
};
}
}
#endif
