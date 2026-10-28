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
#include <fdbus/CGenericUdpSession.h>
#include <fdbus/CGenericUdpSocket.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CBaseSysDep.h>
//#include <fdbus/CGenericUdpSession.h>
#include <platform/socket/linux/CLinuxSocket.h>
#include <fdbus/CFdbContext.h>

namespace ipc {
namespace fdbus {
bool CGenericUdpSocket::connect(const char *url)
{
    if (!url || (url[0] == '\0') || !mSession.get())
    {
        return false;
    }

    CFdbSocketAddr addr;
    if (!CBaseSocketFactory::parseUrl(url, addr))
    {
        return false;
    }
    if (addr.mType != FDB_SOCKET_UDP)
    {
        return false;
    }

    auto udp_socket_imp = fdb_dynamic_cast_if_available<CUDPTransportSocket *>(mSession->getSocket());
    udp_socket_imp->setUDPDestAddr(addr.mAddr.c_str(), addr.mPort);
    return true;
}

FdbScktSessionPtr &CGenericUdpSocket::bind(const char *url, CBaseWorker *worker)
{
    CFdbSocketAddr addr;
    if (url && (url[0] != '\0'))
    {
        if (!CBaseSocketFactory::parseUrl(url, addr))
        {
            return mEmptyPtr;
        }
        if (addr.mType != FDB_SOCKET_UDP)
        {
            return mEmptyPtr;
        }
    }
    else
    {
        addr.mType = FDB_SOCKET_UDP;
    }

    mSocketImp = CBaseSocketFactory::createUDPSocket(addr);
    if (!mSocketImp)
    {
        return mEmptyPtr;
    }
    CSocketImp *socket_imp = 0;
    int32_t retries = FDB_ADDRESS_BIND_RETRY_NR;
    do {
        socket_imp = mSocketImp->bind();
        if (socket_imp)
        {
            break;
        }

        sysdep_sleep(FDB_ADDRESS_BIND_RETRY_INTERVAL);
    } while (--retries > 0);
    if (socket_imp)
    {
        CGenericSession *session = onCreateSession(socket_imp);
        if (session)
        {
            auto &sptr = registerSession(session);
            session->attach(worker ? worker : FDB_CONTEXT);
            onPeerOnline(sptr);
            return sptr;
        }
    }
    return mEmptyPtr;
}

FdbScktSessionPtr &CGenericUdpSocket::findSession(CGenericSession *session)
{
    if (mSession.get() == session)
    {
        return mSession;
    }
    return mEmptyPtr;
}

FdbScktSessionPtr &CGenericUdpSocket::registerSession(CGenericSession *session)
{
    mSession.reset(session);
    return mSession;
}

void CGenericUdpSocket::unregisterSession(CGenericSession *session)
{
    if (mSession.get() == session)
    {
        mSession.reset();
    }
}

CGenericSession *CGenericUdpSocket::onCreateSession(CSocketImp *sock_imp)
{
    return new CGenericUdpSession(this, sock_imp);
}
}
}

