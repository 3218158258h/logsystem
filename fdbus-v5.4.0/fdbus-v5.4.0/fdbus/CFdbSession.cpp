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

#include <fdbus/CFdbSession.h>
#include <utils/CFdbUDPSession.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CLogProducer.h>
#include <fdbus/CSocketImp.h>
#include <fdbus/CFdbRawMsgBuilder.h>
#include <utils/Log.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CBaseSocketFactory.h>

namespace ipc {
namespace fdbus {
#define FDB_SEND_RETRIES (1024 * 10)
#define FDB_SEND_DELAY 2
#define FDB_SEND_MAX_RECURSIVE 128

#define FDB_RECV_RETRIES FDB_SEND_RETRIES
#define FDB_RECV_DELAY FDB_SEND_DELAY

CFdbSession::CFdbSession(FdbSessionId_t sid, CFdbSessionContainer *container, CSocketImp *socket)
    : CBaseSession(sid, container, socket)
    , mSecurityLevel(FDB_SECURITY_LEVEL_NONE)
    , mRecursiveDepth(0)
    , mPid(0)
    , mPeerUDPIpDigit(0)
    , mPeerUDPPort(FDB_INET_PORT_INVALID)
{
    if (mContainer->owner()->enableAysncRead())
    {
        mInputChunk.enable(true);
    }
}

CFdbSession::~CFdbSession()
{
    auto &sn_generator = mPendingMsgTable.getContainer();
    while (!sn_generator.empty())
    {
        auto it = sn_generator.begin();
        terminateMessage(it->second, FDB_ST_PEER_VANISH,
                         "TCP/UDS message is destroyed due to broken connection.");
        mPendingMsgTable.deleteEntry(it);
    }
    
    mContainer->owner()->deleteConnectedSession(this);
    mContainer->owner()->unsubscribeSession(this);
    
    if (mSocket)
    {
        delete mSocket;
        mSocket = 0;
    }
    descriptor(0);

    mContainer->callSessionDestroyHook(this);
}

bool CFdbSession::sendMessage(const uint8_t *buffer, int32_t size)
{
    if (fatalError() || !buffer)
    {
        return false;
    }

    int32_t cnt = 0;
    int32_t retries = FDB_SEND_RETRIES;
    mRecursiveDepth++;
    while (1)
    {
        cnt = mSocket->send((uint8_t *)buffer, size);
        if (cnt < 0)
        {
            break;
        }
        buffer += cnt;
        size -= cnt;
        retries--;
        if ((size <= 0) || (retries <= 0))
        {
            break;
        }
        if (mRecursiveDepth < FDB_SEND_MAX_RECURSIVE)
        {
            worker()->dispatchInput(FDB_SEND_DELAY >> 1);
            sysdep_sleep(FDB_SEND_DELAY >> 1);
        }
        else
        {
            // Sorry just slow down...
            sysdep_sleep(FDB_SEND_DELAY);
        }
    }
    mRecursiveDepth--;

    if ((cnt < 0) || (size > 0))
    {
        if (cnt < 0)
        {
            LOG_E("CFdbSession: error or peer drops when writing %d bytes!\n", size);
        }
        else
        {
            LOG_E("CFdbSession: fail to write %d bytes!\n", size);
        }
        fatalError(true);
        return false;
    }

    return true;
}

bool CFdbSession::sendMessage(CFdbMessage *msg)
{
    auto session_qos = qos();

    if (session_qos != FDB_QOS_LOCAL)
    {
        if (msg->qos() == FDB_QOS_BEST_EFFORTS)
        {
            auto udp_session = mContainer->getUDPSession();
            return udp_session ? udp_session->sendMessage(msg, mPeerUDPIpDigit, mPeerUDPPort) : false;
        }

        if ((msg->qos() == FDB_QOS_SECURE_RELIABLE) && (session_qos == FDB_QOS_RELIABLE))
        {   // secure message should not be sent via insecure channel!
            return false;
        }
    }

    doStatistics(msg->type(), msg->flag(), mStatistics.mTx);
    if (!msg->buildHeader(this))
    {
        return false;
    }

    bool ret = true;
    if (mContainer->owner()->enableAysncWrite())
    {
        auto logger = FDB_CONTEXT->getLogger();
        if (logger && msg->isLogEnabled())
        {
            CFdbRawMsgBuilder builder;
            logger->logFDBus(msg, mSenderName.c_str(), mContainer->owner(), session_qos, builder);
            int32_t log_size = builder.bufferSize();
            uint8_t *log_buffer = 0;
            if (log_size)
            {
                log_buffer = new uint8_t[log_size];
                builder.toBuffer(log_buffer, log_size);
            }
            submitOutput(msg->getRawBuffer(), msg->getRawDataSize(), log_buffer, log_size);
        }
        else
        {
            submitOutput(msg->getRawBuffer(), msg->getRawDataSize(), 0, 0);
        }
    }
    else
    {
        if (sendMessage(msg->getRawBuffer(), msg->getRawDataSize()))
        {
            if (msg->isLogEnabled())
            {
                auto logger = FDB_CONTEXT->getLogger();
                if (logger)
                {
                    logger->logFDBus(msg, mSenderName.c_str(), mContainer->owner(), session_qos);
                }
            }
        }
        else
        {
            ret = false;
        }
    }

    return ret;
}

bool CFdbSession::sendMessageRef(CBaseJob::Ptr &ref)
{
    auto msg = castToMessage<CFdbMessage *>(ref);
    auto session_qos = qos();
    if (session_qos != FDB_QOS_LOCAL)
    {
        if (msg->qos() == FDB_QOS_BEST_EFFORTS)
        {
            auto udp_session = mContainer->getUDPSession();
            return udp_session ? udp_session->sendMessageRef(ref) : false;
        }
    }

    return sendMessageSync(ref, msg);
}

bool CFdbSession::receiveData(uint8_t *buf, int32_t size)
{
    int32_t cnt = 0;
    int32_t retries = FDB_RECV_RETRIES;
    while (1)
    {
        cnt = mSocket->recv(buf, size);
        if (cnt < 0)
        {
            break;
        }
        buf += cnt;
        size -= cnt;
        retries--;
        if ((size <= 0) || (retries <= 0))
        {
            break;
        }
        sysdep_sleep(FDB_RECV_DELAY);
    }

    if ((cnt < 0) || (size > 0))
    {
        if (cnt < 0)
        {
            LOG_E("CFdbSession: error or peer drops when reading %d bytes!\n", size);
        }
        else
        {
            LOG_E("CFdbSession: fail to read %d bytes!\n", size);
        }
        fatalError(true);
        return false;
    }

    return true;
}

int32_t CFdbSession::writeStream(const uint8_t *data, int32_t size)
{
    return mSocket->send((uint8_t *)data, size);
}

int32_t CFdbSession::onInputReady()
{
    int32_t size_expected = 0;
    while (1)
    {
        if (fatalError())
        {
            if (mPayloadBuffer)
            {
                delete[] mPayloadBuffer;
                mPayloadBuffer = 0;
            }
            size_expected = 0;
            break;
        }
        if (mPayloadBuffer)
        {
            size_expected = mInputChunk.extractData(mPayloadBuffer, mMsgPrefix.mTotalLength);
            if (size_expected)
            {
                break;
            }

            processPayload();
            if (mPayloadBuffer)
            {
                delete[] mPayloadBuffer;
                mPayloadBuffer = 0;
            }
        }
        else
        {
            size_expected = mInputChunk.getExpectedSize(CFdbMessage::mPrefixSize);
            if (size_expected)
            {
                break;
            }
            parsePrefix(mInputChunk.getBufferHead());
        }
    }
    return size_expected;
}

void CFdbSession::onInput()
{
    if (fatalError())
    {
        goto _quit;
    }
    uint8_t prefix_buffer[CFdbMessage::mPrefixSize];
    if (receiveData(prefix_buffer, sizeof(prefix_buffer)))
    {
        parsePrefix(prefix_buffer);
    }
    if (fatalError())
    {
        goto _quit;
    }
    if (receiveData(mPayloadBuffer + CFdbMessage::mPrefixSize,
                     mMsgPrefix.mTotalLength - CFdbMessage::mPrefixSize))
    {
        processPayload();
    }

_quit:
    if (mPayloadBuffer)
    {
        delete[] mPayloadBuffer;
        mPayloadBuffer = 0;
    }
}

void CFdbSession::onError()
{
    onHup();
}

void CFdbSession::onHup()
{
    auto endpoint = mContainer->owner();
    delete this;
    endpoint->checkAutoRemove();
}

void CFdbSession::doSubscribeReq(CFdbMessageHeader &head, bool subscribe)
{
    auto object_id = head.object_id();
    auto msg = new CFdbMessage(head, this);
    auto object = mContainer->owner()->getObject(msg, true);
    CBaseJob::Ptr msg_ref(msg);
    
    int32_t error_code;
    const char *error_msg;
    if (object)
    {
        // correct the type so that checkLogEnabled() can get correct
        // sender name and receiver name
        msg->type(FDB_MT_BROADCAST);
        msg->checkLogEnabled(object);
        msg->decodeDebugInfo(head);
        const CFdbMsgSubscribeItem *sub_item;
        int32_t ret;
        bool unsubscribe_object = true;
        FDB_BEGIN_FOREACH_SIGNAL_WITH_RETURN(msg, sub_item, ret)
        {
            auto code = sub_item->msg_code();
            const char *topic = 0;
            if (sub_item->has_topic())
            {
                topic = sub_item->topic().c_str();
            }
            
            if (subscribe)
            {
                // if fail on authentication, unable to subscribe
                if (mContainer->owner()->onGetEventAuthentication(msg, this))
                {
                    object->subscribe(this, code, object_id, topic, sub_item->flag());
                }
                else
                {
                    object->unsubscribe(this, code, object_id, topic);
                    LOG_E("CFdbSession: Session %d: Unable to subscribe obj: %d, id: %d, topic: %s. Fail in security check!\n",
                           mSid, object_id, code, topic);
                    if (ret >= 0)
                    {
                        ret = -2;
                    }
                }
            }
            else
            {
                object->unsubscribe(this, code, object_id, topic);
                unsubscribe_object = false;
            }
        }
        FDB_END_FOREACH_SIGNAL_WITH_RETURN()

        if (ret < 0)
        {
            if (ret == -2)
            {
                error_code = FDB_ST_AUTHENTICATION_FAIL;
                error_msg = "Authentication failed for some events!";
            }
            else
            {
                error_code = FDB_ST_MSG_DECODE_FAIL;
                error_msg = "Not valid CFdbMsgSubscribeList message!";
                LOG_E("CFdbSession: Session %d: Unable to deserialize subscribe message!\n", mSid);
            }
            goto _reply_status;
        }
        else if (subscribe)
        {
            object->doSubscribe(msg_ref);
        }
        else if (unsubscribe_object)
        {   
            object->unsubscribe(object_id); // unsubscribe the whole object
        }
    }
    else
    {
        error_code = FDB_ST_OBJECT_NOT_FOUND;
        error_msg = "Object is not found.";
        goto _reply_status;
        
    }
    return;
    
_reply_status:
    msg->enableLog(true);
    msg->sendStatus(this, error_code, error_msg);
}

const std::string &CFdbSession::getEndpointName() const
{
    return mContainer->owner()->name();
}

void CFdbSession::securityLevel(int32_t level)
{
    mSecurityLevel = level;
}

void CFdbSession::token(const char *token)
{
    mToken = token;
}

bool CFdbSession::hostIp(std::string &host_ip)
{
    CFdbSessionInfo sinfo;
    getSessionInfo(sinfo);
    if (sinfo.mContainerSocket.mAddress->mType == FDB_SOCKET_IPC)
    {
        return false;
    }
    host_ip = sinfo.mConn->mSelfAddress.mAddr;
    return true;
}

bool CFdbSession::peerIp(std::string &host_ip)
{
    CFdbSessionInfo sinfo;
    getSessionInfo(sinfo);
    if (sinfo.mContainerSocket.mAddress->mType == FDB_SOCKET_IPC)
    {
        return false;
    }
    host_ip = sinfo.mConn->mPeerIp;
    return true;

}

bool CFdbSession::connected(const CFdbSocketAddr &addr)
{
    bool already_connected = false;
    const CFdbSocketAddr &session_addr = mSocket->getAddress();
    const CFdbSocketConnInfo &conn_info = mSocket->getConnectionInfo();
    if (session_addr.mType == addr.mType)
    {
        if (session_addr.mType == FDB_SOCKET_IPC)
        {
            if (session_addr.mAddr == addr.mAddr)
            {
                already_connected = true;
            }
        }
        else
        {
            if ((conn_info.mPeerIp == addr.mAddr) &&
                (conn_info.mPeerPort == addr.mPort))
            {
                already_connected = true;
            }
        }
    }
    return already_connected;
}

bool CFdbSession::bound(const CFdbSocketAddr &addr)
{
    bool already_connected = false;
    const CFdbSocketAddr &session_addr = mSocket->getAddress();
    if (session_addr.mType == addr.mType)
    {
        if (session_addr.mType == FDB_SOCKET_IPC)
        {
            if (session_addr.mAddr == addr.mAddr)
            {
                already_connected = true;
            }
        }
        else if ((session_addr.mAddr == addr.mAddr) &&
            (session_addr.mSecure == addr.mSecure))
        {
            if ((session_addr.mPort <= FDB_INET_PORT_AUTO) ||
                (session_addr.mPort == addr.mPort))
            {
                already_connected = true;
            }
        }
    }
    return already_connected;
}

EFdbQOS CFdbSession::qos()
{
    if (mContainer->mSocket->getAddress().mType == FDB_SOCKET_IPC)
    {
        return FDB_QOS_LOCAL;
    }
    else
    {
        return mSocket->isSecure() ? FDB_QOS_SECURE_RELIABLE : FDB_QOS_RELIABLE;
    }
}

void CFdbSession::scanLifeTime()
{
    int32_t force_terminated = 0;
    auto &container = mPendingMsgTable.getContainer();
    for (auto it = container.begin(); it != container.end();)
    {
        auto &msg_ref = it->second;
        auto the_it = it;
        ++it;
        auto msg = castToMessage<CFdbMessage *>(msg_ref);
        msg->incLifeTime();
        if (msg->lifeTime() > FDB_MAX_MSG_LIFETIME)
        {
            terminateMessage(msg_ref, FDB_ST_TIMEOUT,
                             "Message is destroyed since no reply from remote.");
            mPendingMsgTable.deleteEntry(the_it);
            force_terminated++;
        }
    }
    if (force_terminated)
    {
        LOG_E("CFdbSession: Reply is cancelled for %s due to no response from server!\n",
                mContainer->owner()->name().c_str());
    }
    auto queued_output_msg = mOutputChunkList.size();
    if (queued_output_msg > 1000)
    {
        LOG_E("CFdbSession: Very high pressure for transmission in %s. Queued size: %u\n",
                mContainer->owner()->name().c_str(), (uint32_t)queued_output_msg);
    }
}

void CFdbSession::setUDPDestAddr(const char *ip_addr, int32_t port)
{
    if (ip_addr)
    {
        mPeerUDPIp = ip_addr;
        CBaseSocketFactory::parseIp(ip_addr, mPeerUDPIpDigit);
    }
    if (FDB_VALID_PORT(port))
    {
        mPeerUDPPort = port;
    }
}

}
}
