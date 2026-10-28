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

#include <fdbus/CGenericServerSocket.h>
#include <fdbus/CGenericTcpSession.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CBaseSysDep.h>
#include <fdbus/CFdbContext.h>

namespace ipc {
namespace fdbus {
CGenericServerSocket::CGenericServerSocket(ISocketEventHandle *event_handle)
    : CBaseFdWatch(-1, POLLIN)
    , CGenericSocket(event_handle)
    , mSocketImp(0)
{}

bool CGenericServerSocket::bind(const char *url, CBaseWorker *worker)
{
    CFdbSocketAddr addr;
    if (!CBaseSocketFactory::parseUrl(url, addr))
    {
        return false;
    }
    if (addr.mType == FDB_SOCKET_SVC)
    {
        return false;
    }
    mSocketImp = CBaseSocketFactory::createServerSocket(addr);
    if (!mSocketImp)
    {
        return false;
    }

    int32_t retries = FDB_ADDRESS_BIND_RETRY_NR;
    do
    {
        if (mSocketImp->bind())
        {
            break;
        }
    
        sysdep_sleep(FDB_ADDRESS_BIND_RETRY_INTERVAL);
    } while (--retries > 0);
    
    if (retries > 0)
    {
        descriptor(mSocketImp->getFd());
        attach(worker ? worker : FDB_CONTEXT);
        return true;
    }
    return false;
}

void CGenericServerSocket::onInput()
{
    if (!mSocketImp)
    {
        return;
    }
    auto sock_imp = mSocketImp->accept(mScktParams);
    if (sock_imp)
    {
        auto session = onCreateSession(sock_imp);
        if (session)
        {
            auto &sptr = registerSession(session);
            session->attach(worker());
            onPeerOnline(sptr);
        }
    }
}

FdbScktSessionPtr &CGenericServerSocket::registerSession(CGenericSession *session)
{
    {
        std::lock_guard<std::mutex> _l(mSessionTblLock);
        FdbScktSessionPtr sptr(session);
        mSessionTbl.push_back(sptr);
    }
    return mSessionTbl.back();
}

void CGenericServerSocket::unregisterSession(CGenericSession *session)
{
    std::lock_guard<std::mutex> _l(mSessionTblLock);
    for (auto it = mSessionTbl.begin(); it != mSessionTbl.end(); ++it)
    {
        if (it->get() == session)
        {
            mSessionTbl.erase(it);
            break;
        }
    }
}

FdbScktSessionPtr &CGenericServerSocket::findSession(CGenericSession *session)
{
    std::lock_guard<std::mutex> _l(mSessionTblLock);
    for (auto it = mSessionTbl.begin(); it != mSessionTbl.end(); ++it)
    {
        if (it->get() == session)
        {
            return *it;
        }
    }
    return mEmptyPtr;
}

void CGenericServerSocket::getConnectedSessions(CGenericServerSocket::tSessionContainer &sessions)
{
    std::lock_guard<std::mutex> _l(mSessionTblLock);
    for (auto it = mSessionTbl.begin(); it != mSessionTbl.end(); ++it)
    {
        sessions.push_back(*it);
    }
}

CGenericSession *CGenericServerSocket::onCreateSession(CSocketImp *sock_imp)
{
    return new CGenericTcpSession(this, sock_imp);
}

}
}
