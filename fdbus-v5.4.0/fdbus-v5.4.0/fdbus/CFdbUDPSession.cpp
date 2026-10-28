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

#include <utils/CFdbUDPSession.h>
#include <fdbus/CFdbSessionContainer.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CLogProducer.h>
#include <fdbus/CSocketImp.h>
#include <fdbus/CFdbMessage.h>
#include <platform/socket/linux/CLinuxSocket.h>
#include <utils/Log.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CFdbSession.h>

namespace ipc {
namespace fdbus {
#define FDB_MAX_UDP_SIZE        65535

CFdbUDPSession::CFdbUDPSession(CFdbSessionContainer *container, CSocketImp *socket)
    : CBaseSession(FDB_INVALID_ID, container, socket)
{
}

CFdbUDPSession::~CFdbUDPSession()
{
    auto &sn_generator = mPendingMsgTable.getContainer();
    while (!sn_generator.empty())
    {
        auto it = sn_generator.begin();
        terminateMessage(it->second, FDB_ST_PEER_VANISH,
                         "UDP message is destroyed due to broken connection.");
        mPendingMsgTable.deleteEntry(it);
    }

    mContainer->mUDPSession = 0;
    if (mContainer->mUDPSocket)
    {
        delete mContainer->mUDPSocket;
        mContainer->mUDPSocket = 0;
    }
    if (mSocket)
    {
        delete mSocket;
        mSocket = 0;
    }
    descriptor(0);
}

bool CFdbUDPSession::sendMessage(CFdbMessage *msg, tFdbIpV4 peer_ip, int32_t peer_port)
{
    if (!msg->buildHeader(this))
    {
        return false;
    }
    doStatistics(msg->type(), msg->flag(), mStatistics.mTx);
    if (mSocket->send(msg->getRawBuffer(), msg->getRawDataSize(), peer_ip, peer_port))
    {
        if (msg->isLogEnabled())
        {
            auto logger = FDB_CONTEXT->getLogger();
            if (logger)
            {
                logger->logFDBus(msg, 0, mContainer->owner(), qos());
            }
        }
        return true;
    }
    return false;
}

bool CFdbUDPSession::sendMessage(CFdbMessage* msg)
{
    auto session = msg->getSession();
    if (session)
    {
        tFdbIpV4 peer_ip;
        int32_t peer_port;
        session->getUDPDestAddr(peer_ip, peer_port);
        return sendMessage(msg, peer_ip, peer_port);
    }
    return false;
}

bool CFdbUDPSession::sendMessageRef(CBaseJob::Ptr &ref)
{
    auto msg = castToMessage<CFdbMessage *>(ref);
    if (!msg)
    {
        return false;
    }

    return sendMessageSync(ref, msg);
}

void CFdbUDPSession::onInput()
{
    if (fatalError())
    {
        if (mPayloadBuffer)
        {
            delete[] mPayloadBuffer;
            mPayloadBuffer = 0;
        }
        return;
    }

    try
    {
        mPayloadBuffer = new uint8_t[FDB_MAX_UDP_SIZE];
    }
    catch (...)
    {
        LOG_E("CFdbUDPSession: Session %d: Unable to allocate buffer of size %d!\n",
                mSid, FDB_MAX_UDP_SIZE);
        fatalError(true);
        if (mPayloadBuffer)
        {
            delete[] mPayloadBuffer;
            mPayloadBuffer = 0;
        }
        return;
    }
    tFdbIpV4 peer_ip;
    int32_t peer_port;
    auto data_size = mSocket->recv(mPayloadBuffer, FDB_MAX_UDP_SIZE, peer_ip, peer_port);
    if (data_size < CFdbMessage::mPrefixSize)
    {
        fatalError(true);
        if (mPayloadBuffer)
        {
            delete[] mPayloadBuffer;
            mPayloadBuffer = 0;
        }
        return;
    }
    parsePrefix(mPayloadBuffer);
    if (fatalError())
    {
        if (mPayloadBuffer)
        {
            delete[] mPayloadBuffer;
            mPayloadBuffer = 0;
        }
        return;
    }

    auto tcp_session = mContainer->findTcpSession(this, peer_ip, peer_port);
    if (tcp_session)
    {
        mSid = tcp_session->sid();
    }
    processPayload();

    if (mPayloadBuffer)
    {
        delete[] mPayloadBuffer;
        mPayloadBuffer = 0;
    }
}

void CFdbUDPSession::onError()
{
    onHup();
}

void CFdbUDPSession::onHup()
{
    delete this;
}

void CFdbUDPSession::setUDPDestAddr(const char *ip_addr, int32_t port)
{
    auto udp_socket_imp = fdb_dynamic_cast_if_available<CUDPTransportSocket *>(mSocket);
    udp_socket_imp->setUDPDestAddr(ip_addr, port);
}

const std::string &CFdbUDPSession::getUDPDestAddr(int32_t &port) const
{
    auto udp_socket_imp = fdb_dynamic_cast_if_available<CUDPTransportSocket *>(mSocket);
    return udp_socket_imp->getUDPDestAddr(port);
}
void CFdbUDPSession::getUDPDestAddr(tFdbIpV4 &peer_ip, int32_t &peer_port) const
{
    auto udp_socket_imp = fdb_dynamic_cast_if_available<CUDPTransportSocket *>(mSocket);
    return udp_socket_imp->getUDPDestAddr(peer_ip, peer_port);
}

EFdbQOS CFdbUDPSession::qos()
{
    return mContainer->isSecure() ? FDB_QOS_SECURE_BEST_EFFORTS : FDB_QOS_BEST_EFFORTS;
}
}
}
