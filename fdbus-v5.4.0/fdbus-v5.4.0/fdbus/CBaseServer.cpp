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

#include <fdbus/CBaseServer.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbSession.h>
#include <fdbus/CBaseSocketFactory.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CFdbIfNameServer.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
CServerSocket::CServerSocket(CBaseServer *owner
                             , FdbSocketId_t skid
                             , CServerSocketImp *socket
                             , bool secure)
    : CBaseFdWatch(-1, POLLIN)
    , CFdbSessionContainer(skid, owner, socket, FDB_INET_PORT_INVALID, secure)
{
}

CServerSocket::~CServerSocket()
{
}

void CServerSocket::onInput()
{
    auto socket = fdb_dynamic_cast_if_available<CServerSocketImp *>(mSocket);
    CBaseSocket::CSocketParams params(
            (mSocket->getAddress().mType == FDB_SOCKET_IPC) ?
                mOwner->enableIpcBlockingMode() : mOwner->enableTcpBlockingMode(),
            mOwner->getKeepAliveInterval(),
            mOwner->getKeepAliveRetries(),
            mSecure ? mOwner->getSSLContext() : 0
        );
    auto sock_imp = socket->accept(params);
    if (sock_imp)
    {
        auto session = new CFdbSession(FDB_INVALID_ID, this, sock_imp);
        session->attach(worker());
        if (!mOwner->addConnectedSession(this, session))
        {
            delete session;
        }
    }
}

bool CServerSocket::bind(CBaseWorker *worker)
{
    auto socket = fdb_dynamic_cast_if_available<CServerSocketImp *>(mSocket);
    int32_t retries = FDB_ADDRESS_BIND_RETRY_NR;
    do
    {
        if (socket->bind())
        {
            break;
        }

        sysdep_sleep(FDB_ADDRESS_BIND_RETRY_INTERVAL);
    } while (--retries > 0);

    if (retries > 0)
    {
        descriptor(socket->getFd());
        attach(worker);
        return true;
    }
    return false;
}

CBaseServer::CBaseServer(const char *name, CBaseWorker *worker, CFdbBaseContext *context)
    : CBaseEndpoint(name, worker, context, FDB_OBJECT_ROLE_SERVER)
{
}

CBaseServer::~CBaseServer()
{
}

class CBindServerJob : public CMethodJob<CBaseServer>
{
public:
    CBindServerJob(CBaseServer *server, M method, FdbSocketId_t &skid, const char *url)
        : CMethodJob<CBaseServer>(server, method, JOB_FORCE_RUN)
        , mSkId(skid)
    {
        if (url)
        {
            mUrl = url;
        }
    }
    FdbSocketId_t &mSkId;
    std::string mUrl;
};
FdbSocketId_t CBaseServer::bind(const char *url)
{
    FdbSocketId_t skid = FDB_INVALID_ID;
    mContext->sendSyncEndeavor(
        new CBindServerJob(this, &CBaseServer::cbBind, skid, url), 0, true);
    return skid;
}

void CBaseServer::cbBind(CMethodJob<CBaseServer> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CBindServerJob *>(job);
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
    auto sk = doBind(url);
    if (sk)
    {
        the_job->mSkId = sk->skid();
    }
    else
    {
        the_job->mSkId = FDB_INVALID_ID;
    }
}


CServerSocket *CBaseServer::doBind(const char *url, int32_t udp_port)
{
    CFdbSocketAddr addr;
    EFdbSocketType skt_type;
    const char *server_name = 0;
    FdbInstanceId_t instance_id = FDB_DEFAULT_INSTANCE;

    if (url)
    {
        if (!CBaseSocketFactory::parseUrl(url, addr))
        {
            LOG_E("CBaseServer: unable to parse url: %s!\n", url);
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
        if (server_name)
        {
            mApiSecurity.importSecLevel(server_name);
        }
        requestServiceAddress(server_name, instance_id);
        return 0;
    }

    if ((addr.mType != FDB_SOCKET_IPC) && addr.mSecure && !TCPSecureEnabled())
    {
        LOG_I("CBaseServer: unable to connect to %s since security is not enabled!\n", url);
        return 0;
    }

    auto session = bound(addr);
    if (session) /* If the address is already bound, do nothing */
    {
        return fdb_dynamic_cast_if_available<CServerSocket *>(session->container());
    }

    auto server_imp = CBaseSocketFactory::createServerSocket(addr);
    if (server_imp)
    {
        FdbSocketId_t skid = allocateEntityId();
        auto sk = new CServerSocket(this, skid, server_imp, addr.mSecure);
        addSocket(sk);
        sk->bindUDPSocket(0, udp_port);

        if (sk->bind(mContext))
        {
            return sk;
        }
        else
        {
            delete sk;
        }
    }
    return 0;
}

class CUnbindServerJob : public CMethodJob<CBaseServer>
{
public:
    CUnbindServerJob(CBaseServer *server, M method, FdbSocketId_t &skid)
        : CMethodJob<CBaseServer>(server, method, JOB_FORCE_RUN)
        , mSkId(skid)
    {}
    FdbSocketId_t mSkId;
};

void CBaseServer::cbUnbind(CMethodJob<CBaseServer> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CUnbindServerJob *>(job);
    if (!the_job)
    {
        return;
    }

    doUnbind(the_job->mSkId);
    if (!fdbValidFdbId(the_job->mSkId))
    {
        releaseServiceAddress();
        enableMigrate(false);
        // From now on, there will be no jobs migrated to worker thread. Applying a
        // flush to worker thread to ensure no one refers to the object.
    }
}

void CBaseServer::doUnbind(FdbSocketId_t skid)
{
    deleteSocket(skid);
}

void CBaseServer::unbind(FdbSocketId_t skid)
{
    mContext->sendSyncEndeavor(
            new CUnbindServerJob(this, &CBaseServer::cbUnbind, skid), 0, true);
}

void CBaseServer::onSidebandInvoke(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    switch (msg->code())
    {
        case FDB_SIDEBAND_AUTH:
        {
            FdbAuthentication authen;
            CFdbParcelableParser parser(authen);
            if (!msg->deserialize(parser))
            {
                msg->status(msg_ref, FDB_ST_MSG_DECODE_FAIL);
                return;
            }

            int32_t security_level = FDB_SECURITY_LEVEL_NONE;
            auto token = "";
            if (authen.has_token_list() && !authen.token_list().tokens().empty())
            {
                const auto &tokens = authen.token_list().tokens();
                // only use the first token in case more than 1 tokens are received
                token = tokens.pool().begin()->c_str();
                security_level = checkSecurityLevel(token);
            }

            auto session = getSession(msg->session());
            if (!session)
            {
                return;
            }

            session->securityLevel(security_level);
            session->token(token);
            LOG_I("CBaseServer: security is set: session: %d, level: %d.\n",
                    session->sid(), security_level);
        }
        break;
        default:
            CBaseEndpoint::onSidebandInvoke(msg_ref);
        break;
    }
}

void CBaseServer::prepareDestroy()
{
    unbind();
    CBaseEndpoint::prepareDestroy();
}
}
}
