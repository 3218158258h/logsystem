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
#include <fdbus/CBaseNameProxy.h>
#include <fdbus/CBaseSocketFactory.h>
#include <utils/CNsConfig.h>
#include <fdbus/CFdbBaseContext.h>

namespace ipc {
namespace fdbus {
CBaseNameProxy::CBaseNameProxy()
    : CBaseClient("")
    , mEnableReconnectToNS(true)
    , mConnectTimer(this)
{
    mConnectTimer.attach(mContext, false);
}

void CBaseNameProxy::CConnectTimer::fire()
{
    enableOneShot(CNsConfig::getNsReconnectInterval());
}

void CBaseNameProxy::onConnectTimer(CMethodLoopTimer<CBaseNameProxy> *timer)
{
#if 0
    LOG_E("CBaseNameProxy: Reconnecting to name server...\n");
#endif
    if (mEnableReconnectToNS)
    {
        connectToNameServer();
    }
}

void CBaseNameProxy::onOnline(const CFdbOnlineInfo &info)
{
    mConnectTimer.disable();
}
void CBaseNameProxy::onOffline(const CFdbOnlineInfo &info)
{
    if (mEnableReconnectToNS)
    {
        mConnectTimer.fire();
    }
}

bool CBaseNameProxy::connectToNameServer()
{
    if (connected())
    {
        return true;
    }
    // timer will stop if connected since onOnline() will be called
    // upon success
    doConnect(mNsUrl.c_str());
    if (!connected())
    {
        mConnectTimer.fire();
        return false;
    }

    return true;
}

CBaseNameProxy::CConnectTimer::CConnectTimer(CBaseNameProxy *proxy)
    : CMethodLoopTimer<CBaseNameProxy>(CNsConfig::getNsReconnectInterval(), false,
                                   proxy, &CBaseNameProxy::onConnectTimer)
{
}

}
}
