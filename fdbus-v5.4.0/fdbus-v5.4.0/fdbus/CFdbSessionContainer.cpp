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

#include <fdbus/CFdbSessionContainer.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CBaseEndpoint.h>
#include <fdbus/CBaseSocketFactory.h>
#include <utils/CFdbUDPSession.h>
#include <fdbus/CFdbSession.h>
#include <utils/Log.h>
#include <algorithm>

namespace ipc {
namespace fdbus {
CFdbSessionContainer::CFdbSessionContainer(FdbSocketId_t skid , CBaseEndpoint *owner,
                                           CBaseSocket *tcp_socket, int32_t udp_port, bool secure)
    : mSkid(skid)
    , mOwner(owner)
    , mSocket(tcp_socket)
    , mSecure(secure)
    , mEnableSessionDestroyHook(true)
    , mUDPSocket(0)
    , mUDPSession(0)
    , mPendingUDPPort(udp_port)
    , mInterfaceId(FDB_INVALID_ID)
{
}

CFdbSessionContainer::~CFdbSessionContainer()
{
    mOwner->deleteSession(this);
    if (!mConnectedSessionTable.empty())
    {
        LOG_E("CFdbSessionContainer: Untracked sessions are found!!!\n");
    }

    CFdbSessionContainer *self = 0;
    auto it = mOwner->retrieveEntry(mSkid, self);
    if (self)
    {
        mOwner->deleteEntry(it);
    }
    if (mUDPSession)
    {
        delete mUDPSession;
        mUDPSession = 0;
    }
    if (mUDPSocket)
    {
        delete mUDPSocket;
        mUDPSocket = 0;
    }
    if (mSocket)
    {
        delete mSocket;
        mSocket = 0;
    }
}

CFdbSession *CFdbSessionContainer::getDefaultSession()
{
    auto it = mConnectedSessionTable.begin();
    return (it == mConnectedSessionTable.end()) ? 0 : *it;
}

void CFdbSessionContainer::addSession(CFdbSession *session)
{
    for (auto it = mConnectedSessionTable.begin();
            it != mConnectedSessionTable.end(); ++it)
    {
        if (*it == session)
        {
            return;
        }
    }

    mConnectedSessionTable.push_back(session);
}

void CFdbSessionContainer::removeSession(CFdbSession *session)
{
    fdb_remove_value_from_container(mConnectedSessionTable, session);
}

void CFdbSessionContainer::callSessionDestroyHook(CFdbSession *session)
{
    if (mEnableSessionDestroyHook)
    {
        onSessionDeleted(session);
    }
}

bool CFdbSessionContainer::getSocketInfo(CFdbSocketInfo &info)
{
    info.mAddress = &mSocket->getAddress();
    return true;
}

bool CFdbSessionContainer::bindUDPSocket(const char *ip_address, int32_t udp_port)
{
    if (!mOwner->UDPEnabled())
    {
        return false;
    }
    if (udp_port == FDB_INET_PORT_INVALID)
    {
        udp_port = mPendingUDPPort;
    }
    else
    {
        mPendingUDPPort = udp_port;
    }

    if (!mSocket || (udp_port == FDB_INET_PORT_INVALID))
    {
        return false;
    }
    const CFdbSocketAddr &tcp_addr = mSocket->getAddress();
    if (tcp_addr.mType == FDB_SOCKET_IPC)
    {
        return false;
    }

    if (mUDPSocket)
    {
        const CFdbSocketAddr &udp_addr = mUDPSocket->getAddress();
        if (mUDPSession)
        {
            if (udp_addr.mPort == udp_port)
            {
                return true;
            }
            delete mUDPSession;
            mUDPSession = 0;
        }
        delete mUDPSocket;
        mUDPSocket = 0;
    }

    CFdbSocketAddr udp_addr;
    if (ip_address)
    {
        udp_addr.mAddr = ip_address;
    }
    else
    {
        udp_addr.mAddr = tcp_addr.mAddr;
    }
    udp_addr.mType = FDB_SOCKET_UDP;
    udp_addr.mPort = udp_port;
    udp_addr.mSecure = tcp_addr.mSecure;
    auto udp_socket = CBaseSocketFactory::createUDPSocket(udp_addr);
    if (udp_socket)
    {
        CSocketImp *socket_imp = 0;
        int32_t retries = FDB_ADDRESS_BIND_RETRY_NR;
        do {
            socket_imp = udp_socket->bind();
            if (socket_imp)
            {
                break;
            }

            sysdep_sleep(FDB_ADDRESS_BIND_RETRY_INTERVAL);
        } while (--retries > 0);

        if (socket_imp)
        {
            mUDPSocket = udp_socket;
            mUDPSession = new CFdbUDPSession(this, socket_imp);
            mUDPSession->attach(mOwner->context());
            return true;
        }
        else
        {
            LOG_E("CFdbSessionContainer: fail to bind socket: udp://%s:%d\n",
                    udp_addr.mAddr.c_str(), udp_addr.mPort);
            delete udp_socket;
        }
    }
    return false;
}

CFdbSession *CFdbSessionContainer::findTcpSession(CFdbUDPSession *udp_session,
                                                  tFdbIpV4 peer_ip,
                                                  int32_t peer_port)
{
    for (auto it = mConnectedSessionTable.begin();
            it != mConnectedSessionTable.end(); ++it)
    {
        auto session = *it;
        tFdbIpV4 session_udp_ip;
        int32_t session_peer_port;
        session->getUDPDestAddr(session_udp_ip, session_peer_port);
        if ((peer_ip == session_udp_ip) && (peer_port == session_peer_port))
        {
            return session;
        }
    }
    return 0;
}

bool CFdbSessionContainer::getUDPSocketInfo(CFdbSocketInfo &info)
{
    if (mUDPSession && mUDPSession->getSocket())
    {
        // can get from session as well
        info.mAddress = &mUDPSession->getSocket()->getAddress();
        return true;
    }
    return false;
}

CFdbSession *CFdbSessionContainer::connectedSession(const CFdbSocketAddr &addr)
{
    for (auto it = mConnectedSessionTable.begin();
            it != mConnectedSessionTable.end(); ++it)
    {
        auto session = *it;
        if (session->connected(addr))
        {
            return session;
        }
    }
    return 0;
}

CFdbSession *CFdbSessionContainer::connectedSession(EFdbSecureType sec_type)
{
    for (auto it = mConnectedSessionTable.begin();
            it != mConnectedSessionTable.end(); ++it)
    {
        auto session = *it;
        if (sec_type == FDB_SEC_INSECURE)
        {
            if (!session->isSecure())
            {
                return session;
            }
        }
        else if (sec_type == FDB_SEC_SECURE)
        {
            if (session->isSecure())
            {
                return session;
            }
        }
        else
        {
            return session;
        }
    }
    return 0;
}

CFdbSession *CFdbSessionContainer::bound(const CFdbSocketAddr &addr)
{
    for (auto it = mConnectedSessionTable.begin();
            it != mConnectedSessionTable.end(); ++it)
    {
        auto session = *it;
        if (session->bound(addr))
        {
            return session;
        }
    }
    return 0;
}

// Check if peer address of UDP socket is recorded in 
// one of TCP sessions. If found, it means the peer
// address is legal and can be accepted.
int32_t CFdbSessionContainer::checkSecurityLevel()
{
    if (!mUDPSession)
    {
        return FDB_SECURITY_LEVEL_NONE;
    }
    tFdbIpV4 peer_ip;
    int32_t peer_port;
    mUDPSession->getUDPDestAddr(peer_ip, peer_port);
    if (!FDB_VALID_PORT(peer_port))
    {
        return FDB_SECURITY_LEVEL_NONE;
    }

    for (auto it = mConnectedSessionTable.begin();
            it != mConnectedSessionTable.end(); ++it)
    {
        auto session = *it;
        tFdbIpV4 legal_ip;
        int32_t legal_port;
        session->getUDPDestAddr(legal_ip, legal_port);
        if (FDB_VALID_PORT(legal_port))
        {
            if ((legal_ip == peer_ip) && (legal_port == peer_port))
            {
                return session->securityLevel();
            }
        }
    }
    return FDB_SECURITY_LEVEL_UNKNOWN;
}
}
}
