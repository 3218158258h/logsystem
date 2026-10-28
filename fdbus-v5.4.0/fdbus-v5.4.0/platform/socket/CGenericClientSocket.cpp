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

#include <fdbus/CGenericClientSocket.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CBaseSysDep.h>
#include <fdbus/CGenericTcpSession.h>
#include <fdbus/CFdbContext.h>
#include <utils/CNsConfig.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
CGenericClientSocket::CConnectTimer::CConnectTimer(CGenericClientSocket *client)
    : CMethodLoopTimer<CGenericClientSocket>(CNsConfig::getHsReconnectInterval(), false,
                                   client, &CGenericClientSocket::onConnectTimer)
{
}

CGenericClientSocket::CGenericClientSocket(ISocketEventHandle *event_handle)
    : CGenericSocket(event_handle)
    , mSocketImp(0)
    , mConnectTimer(this)
    , mWorker(0)
    , mEnableReconnect(false)
{}

FdbScktSessionPtr &CGenericClientSocket::doConnect()
{
    if (!mWorker)
    {
        return mEmptyPtr;
    }

    int32_t retries = FDB_ADDRESS_CONNECT_RETRY_NR;
    CSocketImp *sock_imp = 0;
    do {
        sock_imp = mSocketImp->connect(mScktParams);
        if (sock_imp)
        {
            break;
        }

        sysdep_sleep(FDB_ADDRESS_CONNECT_RETRY_INTERVAL);
    } while (--retries > 0);

    if (sock_imp)
    {
        CGenericSession *session = onCreateSession(sock_imp);
        if (session)
        {
            auto &sptr = registerSession(session);
            session->attach(mWorker);
            onPeerOnline(sptr);
            return sptr;
        }
    }
    else if (mEnableReconnect)
    {
        mConnectTimer.attach(mWorker, true);
    }

    return mEmptyPtr;
}

FdbScktSessionPtr &CGenericClientSocket::connect(const char *url, CBaseWorker *worker)
{
    CFdbSocketAddr addr;
    if (!CBaseSocketFactory::parseUrl(url, addr))
    {
        return mEmptyPtr;
    }
    if (addr.mType == FDB_SOCKET_SVC)
    {
        return mEmptyPtr;
    }

    mSocketImp = CBaseSocketFactory::createClientSocket(addr);
    if (!mSocketImp)
    {
        return mEmptyPtr;
    }
    mWorker = worker ? worker : FDB_CONTEXT;

    return doConnect();
}

void CGenericClientSocket::onConnectTimer(CMethodLoopTimer<CGenericClientSocket> *timer)
{
    doConnect();
}

FdbScktSessionPtr &CGenericClientSocket::findSession(CGenericSession *session)
{
    if (mSession.get() == session)
    {
        return mSession;
    }
    return mEmptyPtr;
}

FdbScktSessionPtr &CGenericClientSocket::registerSession(CGenericSession *session)
{
    mSession.reset(session);
    return mSession;
}

void CGenericClientSocket::unregisterSession(CGenericSession *session)
{
    if (mSession.get() == session)
    {
        mSession.reset();
    }
}

CGenericSession *CGenericClientSocket::onCreateSession(CSocketImp *sock_imp)
{
    return new CGenericTcpSession(this, sock_imp);
}

void CGenericClientSocket::onPeerOffline(FdbScktSessionPtr &session)
{
    if (mEnableReconnect)
    {
        LOG_E("CGenericClientSocket: Disconnected and reconnecting...\n");
        doConnect();
    }
    CGenericSocket::onPeerOffline(session);
}
}
}

