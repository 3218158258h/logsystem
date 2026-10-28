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

#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CFdbSession.h>
#include <utils/CFdbIfMessageHeader.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
#define FDB_CLIENT_RECONNECT_WAIT_MS    1

CClientSocket::CClientSocket(CBaseClient *owner
                             , FdbSocketId_t skid
                             , CClientSocketImp *socket
                             , const char *host_name
                             , int32_t udp_port
                             , bool secure)
    : CFdbSessionContainer(skid, owner, socket, udp_port, secure)
    , mConnectedHost(host_name ? host_name : "")
{
}

CClientSocket::~CClientSocket()
{
    // so that onSessionDeleted() will not be called upon session destroy
    enableSessionDestroyHook(false);
}

CFdbSession *CClientSocket::connect()
{
    CFdbSession *session = 0;
    auto socket = fdb_dynamic_cast_if_available<CClientSocketImp *>(mSocket);
    int32_t retries = FDB_ADDRESS_CONNECT_RETRY_NR;
    CBaseSocket::CSocketParams params(
            (mSocket->getAddress().mType == FDB_SOCKET_IPC) ?
                mOwner->enableIpcBlockingMode() : mOwner->enableTcpBlockingMode(),
            mOwner->getKeepAliveInterval(),
            mOwner->getKeepAliveRetries(),
            mSecure ? mOwner->getSSLContext() : 0
        );
    CSocketImp *sock_imp;
    do {
        sock_imp = socket->connect(params);
        if (sock_imp)
        {
            break;
        }

        sysdep_sleep(FDB_ADDRESS_CONNECT_RETRY_INTERVAL);
    } while (--retries > 0);

    if (sock_imp)
    {
        session = new CFdbSession(FDB_INVALID_ID, this, sock_imp);
    }
    return session;
}

void CClientSocket::disconnect()
{
    if (mSocket)
    {
        delete mSocket;
        mSocket = 0;
    }
}

void CClientSocket::onSessionDeleted(CFdbSession *session)
{
    auto client = fdb_dynamic_cast_if_available<CBaseClient *>(mOwner);
    
    if (mOwner->reconnectEnabled() && mOwner->reconnectActivated() && client)
    {
        auto url = mSocket->getAddress().mUrl;

        CFdbSessionContainer::onSessionDeleted(session);
        delete this;

        // always connect to server
        if (!client->requestServiceAddress())
        {
            if (FDB_CLIENT_RECONNECT_WAIT_MS)
            {
                sysdep_sleep(FDB_CLIENT_RECONNECT_WAIT_MS);
            }
            if (client->doConnect(url.c_str()))
            {
                LOG_E("CClientSocket: client %s shutdown but reconnected to %s@%s.\n",
                      client->name().c_str(), client->nsName().c_str(), url.c_str());
            }
            else
            {
                LOG_E("CClientSocket: client %s shutdown fail to reconnect to %s@%s.\n",
                      client->name().c_str(), client->nsName().c_str(), url.c_str());
            }
        }
        else
        {
            LOG_E("CClientSocket: client %s shutdown but try to request address of and connect again...\n",
                  client->name().c_str(), client->nsName().c_str());
        }
    }
    else
    {
        CFdbSessionContainer::onSessionDeleted(session);
        delete this;
    }
}

CBaseClient::CBaseClient(const char *name, CBaseWorker *worker, CFdbBaseContext *context)
    : CBaseEndpoint(name, worker, context, FDB_OBJECT_ROLE_CLIENT)
    , mIsLocal(true)
{
}

CBaseClient::~CBaseClient()
{
}

class CConnectClientJob : public CMethodJob<CBaseClient>
{
public:
    CConnectClientJob(CBaseClient *client, M method, int32_t timeout, const char *url, ConnectStatus &status)
        : CMethodJob<CBaseClient>(client, method, JOB_FORCE_RUN)
        , mTimeout(timeout)
        , mStatus(status)
    {
        if (url)
        {
            mUrl = url;
        }
    }
    int32_t mTimeout;
    ConnectStatus &mStatus;
    std::string mUrl;
};
ConnectStatus CBaseClient::connect(const char *url, int32_t timeout)
{
    if (mContext->isSelf() && (timeout != -1))
    {
        return FAILURE;
    }
    ConnectStatus status = FAILURE;
    mContext->sendSyncEndeavor(new CConnectClientJob(
                                this, &CBaseClient::cbConnect, timeout, url, status), timeout, true);
    return status;
}

void CBaseClient::cbConnect(CMethodJob<CBaseClient> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CConnectClientJob *>(job);
    if (!the_job)
    {
        return;
    }

    enableMigrate(true);
    std::string svc_url;
    const char *url;
    if (the_job->mUrl.empty())
    {
        getDefaultSvcUrl(svc_url);
        url = svc_url.c_str();
    }
    else
    {
        url = the_job->mUrl.c_str();
    }

    if (the_job->mTimeout == -1)
    {
        the_job->mStatus = CONNECTING;
    }
    else
    {   // -1 means never wait
        the_job->mStatus = TIMEOUT;
        mConnectJobList.push_back(ref);
    }
    doConnect(url);
}

void CBaseClient::notifyConnectReady(ConnectStatus status)
{
    while (!mConnectJobList.empty())
    {
        auto it = mConnectJobList.begin();
        auto &job = *it;
        auto connect_job = fdb_dynamic_cast_if_available<CConnectClientJob *>(&*job);
        connect_job->mStatus = status;
        connect_job->terminate(job);
        mConnectJobList.erase(it);
    }
}

CClientSocket *CBaseClient::doConnect(const char *url, const char *host_name, int32_t udp_port)
{
    CFdbSocketAddr addr;
    EFdbSocketType skt_type;
    const char *server_name = 0;
    FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE;

    if (url)
    {
        if (!CBaseSocketFactory::parseUrl(url, addr))
        {
            notifyConnectReady(FAILURE);
            LOG_E("CBaseClient: unable to parse url: %s!\n", url);
            return 0;
        }
        skt_type = addr.mType;
        if (skt_type == FDB_SOCKET_SVC)
        {
            server_name = addr.mAddr.c_str();
            instance_id = addr.mPort;
        }
    }
    else
    {
        skt_type = FDB_SOCKET_SVC;
        server_name = 0;
    }

    if (skt_type == FDB_SOCKET_SVC)
    {
        requestServiceAddress(server_name, instance_id);
        return 0;
    }

    if ((addr.mType != FDB_SOCKET_IPC) && addr.mSecure && !TCPSecureEnabled())
    {
        LOG_I("CBaseClient: unable to connect to %s since security is not enabled!\n", url);
        notifyConnectReady(FAILURE);
        return 0;
    }

    // Address connection policy:
    // for the same address, only one secure and one inscure connect;
    // for different addresses, only one for secure and another for insecure
    auto session = connectedSession(addr);
    if (!session)
    {
        session = connectedSession(addr.mSecure ? FDB_SEC_SECURE : FDB_SEC_INSECURE);
    }
    if (session)
    {
        if ((skt_type != FDB_SOCKET_IPC) && (udp_port >= FDB_INET_PORT_AUTO))
        {
            CFdbSocketInfo socket_info;
            if (!session->container()->getUDPSocketInfo(socket_info) ||
                !FDB_VALID_PORT(socket_info.mAddress->mPort))
            {
                session->container()->pendingUDPPort(udp_port);
                updateSessionInfo(session);
            }
        }
        notifyConnectReady(CONNECTED);
        return fdb_dynamic_cast_if_available<CClientSocket *>(session->container());
    }

    auto client_imp = CBaseSocketFactory::createClientSocket(addr);
    if (client_imp)
    {
        FdbSocketId_t skid = allocateEntityId();
        auto sk = new CClientSocket(this, skid, client_imp, host_name, udp_port, addr.mSecure);
        addSocket(sk);

        auto session = sk->connect();
        if (session)
        {
            session->attach(mContext);
            updateSecurityLevel(session);
            if (addConnectedSession(sk, session))
            {
                activateReconnect(true);
                notifyConnectReady(CONNECTED);
                return sk;
            }
            else
            {
                delete session;
                deleteSocket(skid);
                notifyConnectReady(FAILURE);
                return 0;
            }
        }
        else
        {
            deleteSocket(skid);
        }
    }

    notifyConnectReady(FAILURE);
    return 0;
}

class CDisconnectClientJob : public CMethodJob<CBaseClient>
{
public:
    CDisconnectClientJob(CBaseClient *client, M method, FdbSessionId_t &sid)
        : CMethodJob<CBaseClient>(client, method, JOB_FORCE_RUN)
        , mSid(sid)
    {}
    FdbSessionId_t mSid;
};

void CBaseClient::cbDisconnect(CMethodJob<CBaseClient> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CDisconnectClientJob *>(job);
    if (!the_job)
    {
        return;
    }
    
    doDisconnect(the_job->mSid);
    if (!fdbValidFdbId(the_job->mSid))
    {
        activateReconnect(false);
        releaseServiceAddress();
        enableMigrate(false);
        // From now on, there will be no jobs migrated to worker thread. Applying a
        // flush to worker thread to ensure no one refers to the object.
    }
}

void CBaseClient::doDisconnect(FdbSessionId_t sid)
{
    FdbSocketId_t skid = FDB_INVALID_ID;
    
    if (fdbValidFdbId(sid))
    {
        auto session = getSession(sid);
        if (session)
        {
            skid = session->container()->skid();
        }
    }

    deleteSocket(skid);
}

void CBaseClient::disconnect(FdbSessionId_t sid)
{
    mContext->sendSyncEndeavor(
                new CDisconnectClientJob(this, &CBaseClient::cbDisconnect, sid), 0, true);
}

void CBaseClient::updateSecurityLevel(CFdbSession *session)
{
    if (!mTokens.empty())
    {
        FdbAuthentication authen;
        for (auto it = mTokens.begin(); it != mTokens.end(); ++it)
        {
            authen.token_list().add_tokens(*it);
        }
        CFdbParcelableBuilder builder(authen);
        sendSideband(session, FDB_SIDEBAND_AUTH, builder);
    }
}

bool CBaseClient::hostConnected(const char *host_name)
{
    if (!host_name)
    {
        return false;
    }
    auto &containers = getContainer();
    for (auto it = containers.begin(); it != containers.end(); ++it)
    {
        auto sessions = it->second;
        auto client_socket = fdb_dynamic_cast_if_available<CClientSocket *>(sessions);
        if (client_socket)
        {
            if (!client_socket->connectedHost().compare(host_name))
            {
                return true;
            }
        }
    }

    return false;
}

void CBaseClient::prepareDestroy()
{
    disconnect();
    CBaseEndpoint::prepareDestroy();
}

}
}
