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

#include "CBaseHostProxy.h"
#include <fdbus/CFdbContext.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
CBaseHostProxy::CBaseHostProxy(const char *host_url)
    : CBaseClient(0)
    , mConnectTimer(this)
{
    if (host_url)
    {
        mHostUrl = host_url;
    }
    mNotifyHdl.registerCallback(NTF_HEART_BEAT, &CBaseHostProxy::onHeartbeatOk);
    mConnectTimer.attach(mContext, false);
}

bool CBaseHostProxy::connectToHostServer()
{
    if (connected())
    {
        return true;
    }

    // timer will stop if connected since onOnline() will be called
    // upon success
    doConnect(mHostUrl.c_str());
    if (!connected())
    {
        mConnectTimer.startReconnectMode();
        return false;
    }

    LOG_I("CBaseHostProxy: upstream host server %s is connected.\n", mHostUrl.c_str());
    return true;
}

void CBaseHostProxy::onConnectTimer(CMethodLoopTimer<CBaseHostProxy> *timer)
{
    switch (mConnectTimer.mMode)
    {
        case CConnectTimer::MODE_RECONNECT_TO_SERVER:
#if 0
            LOG_I("CBaseHostProxy: Reconnecting to host server...\n");
#endif
            connectToHostServer();
        break;
        case CConnectTimer::MODE_HEARTBEAT:
            LOG_E("CBaseHostProxy: Heartbeat timeout! Disconnecting to all hosts...\n");
            isolate();
            doDisconnect();
        break;
        default:
        break;
    }
}

void CBaseHostProxy::onOnline(const CFdbOnlineInfo &info)
{
    mConnectTimer.disable();
    CFdbMsgSubscribeList subscribe_list;
    addNotifyItem(subscribe_list, NTF_HEART_BEAT);
    subscribe(subscribe_list, 0);
}

void CBaseHostProxy::onOffline(const CFdbOnlineInfo &info)
{
    mConnectTimer.startReconnectMode();
}

void CBaseHostProxy::onHeartbeatOk(CBaseJob::Ptr &msg_ref)
{
    send(REQ_HEARTBEAT_OK);
    mConnectTimer.startHeartbeatMode();
}

void CBaseHostProxy::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    mNotifyHdl.processMessage(this, msg_ref);
}
}
}
