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
#include <fdbus/CBaseSession.h>
#include <utils/Log.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CSocketImp.h>
#include <fdbus/CFdbSessionContainer.h>
#include <fdbus/CBaseEndpoint.h>

namespace ipc {
namespace fdbus {
CBaseSession::CBaseSession(FdbSessionId_t sid, CFdbSessionContainer *container, CSocketImp *socket)
    : CBaseFdWatch(socket->getFd(), POLLIN | POLLHUP | POLLERR)
    , mContainer(container)
    , mSocket(socket)
    , mPayloadBuffer(0)
    , mSid(sid)
{
}

CBaseSession::~CBaseSession()
{
    if (mPayloadBuffer)
    {
        delete[] mPayloadBuffer;
        mPayloadBuffer = 0;
    }
}

void CBaseSession::parsePrefix(const uint8_t *data)
{
    mMsgPrefix.deserialize(data);
    int32_t data_size = mMsgPrefix.mTotalLength - CFdbMessage::mPrefixSize;
    if (data_size < 0)
    {
        fatalError(true);
        return;
    }
    if (mPayloadBuffer)
    {
        return;
    }
    /*
     * The leading CFdbMessage::mPrefixSize bytes are not used; just for
     * keeping uniform structure
     */
    try
    {
        mPayloadBuffer = new uint8_t[mMsgPrefix.mTotalLength];
    }
    catch (...)
    {
        LOG_E("CBaseSession: Session %d: Unable to allocate buffer of size %d!\n",
                sid(), mMsgPrefix.mTotalLength);
        fatalError(true);
    }
}

void CBaseSession::processPayload()
{
    const uint8_t *data = mPayloadBuffer + CFdbMessage::mPrefixSize;
    int32_t size = mMsgPrefix.mTotalLength - CFdbMessage::mPrefixSize;
    if (size < 0)
    {
        fatalError(true);
        return;
    }

    CFdbMessageHeader head;
    CFdbParcelableParser parser(head);
    if (!parser.parse(data, mMsgPrefix.mHeadLength))
    {
        LOG_E("CBaseSession: Session %d: Unable to deserialize message head!\n", sid());
        fatalError(true);
        return;
    }

    auto type = head.type();
    switch (type)
    {
        case FDB_MT_REQUEST:
        case FDB_MT_SIDEBAND_REQUEST:
        case FDB_MT_GET_EVENT:
        case FDB_MT_PUBLISH:
        case FDB_MT_SET_EVENT:
            doStatistics(type, head.flag(), mStatistics.mRx);
            doRequest(head);
            break;
        case FDB_MT_RETURN_EVENT:
        case FDB_MT_REPLY:
        case FDB_MT_SIDEBAND_REPLY:
        case FDB_MT_STATUS:
            doResponse(head);
            break;
        case FDB_MT_BROADCAST:
            doStatistics(type, head.flag(), mStatistics.mRx);
            doBroadcast(head);
            break;
        case FDB_MT_SUBSCRIBE_REQ:
            if (head.code() == FDB_CODE_SUBSCRIBE)
            {
                doSubscribeReq(head, true);
            }
            else if (head.code() == FDB_CODE_UNSUBSCRIBE)
            {
                doSubscribeReq(head, false);
            }
            break;
        default:
            LOG_E("CBaseSession: Message %d: Unknown type!\n", (int32_t)head.serial_number());
            fatalError(true);
            break;
    }
}

void CBaseSession::doRequest(CFdbMessageHeader &head)
{
    auto msg = new CFdbMessage(head, this);
    auto object = mContainer->owner()->getObject(msg, true);
    CBaseJob::Ptr msg_ref(msg);

    if (object)
    {
        msg->checkLogEnabled(object);
        msg->decodeDebugInfo(head);
        switch (head.type())
        {
            case FDB_MT_REQUEST:
                if (mContainer->owner()->onMessageAuthentication(msg, this))
                {
                    object->doInvoke(msg_ref);
                }
                else
                {
                    msg->sendStatus(this, FDB_ST_AUTHENTICATION_FAIL,
                                    "Authentication failed!");
                }
            break;
            case FDB_MT_SIDEBAND_REQUEST:
                try // catch exception to avoid missing of auto-reply
                {
                    object->onSidebandInvoke(msg_ref);
                }
                catch (...)
                {
                }
                // check if auto-reply is required
                msg->autoReply(this, msg_ref, FDB_ST_AUTO_REPLY_OK,
                               "Automatically reply to request.");
            break;
            case FDB_MT_GET_EVENT:
                if (mContainer->owner()->onGetEventAuthentication(msg, this))
                {
                    object->doGetEvent(msg_ref);
                }
                else
                {
                    msg->sendStatus(this, FDB_ST_AUTHENTICATION_FAIL,
                                    "Authentication failed!");
                }
            break;
            case FDB_MT_SET_EVENT:
                if (mContainer->owner()->onSetEventAuthentication(msg, this))
                {
                    object->doSetEvent(msg_ref);
                }
                else
                {
                    msg->sendStatus(this, FDB_ST_AUTHENTICATION_FAIL,
                                    "Authentication failed!");
                }
            break;
            case FDB_MT_PUBLISH:
                if (mContainer->owner()->onPublishAuthentication(msg, this))
                {
                    object->doPublish(msg_ref);
                }
                else
                {
                    msg->sendStatus(this, FDB_ST_AUTHENTICATION_FAIL,
                                    "Authentication failed!");
                }
            break;
            default:
            break;
        }
    }
    else
    {
        msg->enableLog(true);
        msg->sendStatus(this, FDB_ST_OBJECT_NOT_FOUND, "Object is not found.");
    }
}

int32_t CBaseSession::readStream(uint8_t *data, int32_t size)
{
    return mSocket->recv(data, size);
}

bool CBaseSession::isSecure() const
{
    return mContainer->isSecure();
}

void CBaseSession::doStatistics(EFdbMessageType type, uint32_t flag, CStatisticsData &stat_data)
{
    switch (type)
    {
        case FDB_MT_REQUEST:
            if (flag & MSG_FLAG_NOREPLY_EXPECTED)
            {
                stat_data.mSend++;
            }
            else
            {
                if (flag & MSG_FLAG_SYNC_REPLY)
                {
                    stat_data.mSyncRequest++;
                }
                else
                {
                    stat_data.mAsyncRequest++;
                }
            }
        break;
        case FDB_MT_STATUS:
            if (flag & MSG_FLAG_SYNC_REPLY)
            {
                stat_data.mSyncStatus++;
            }
            else
            {
                stat_data.mAsyncStatus++;
            }
        break;
        case FDB_MT_GET_EVENT:
            stat_data.mGetEvent++;
        break;
        case FDB_MT_SET_EVENT:
            stat_data.mSetEvent++;
        break;
        case FDB_MT_REPLY:
            if (flag & MSG_FLAG_SYNC_REPLY)
            {
                stat_data.mSyncReply++;
            }
            else
            {
                stat_data.mAsyncReply++;
            }
        break;
        case FDB_MT_RETURN_EVENT:
            stat_data.mReturnEvent++;
        break;
        case FDB_MT_BROADCAST:
            stat_data.mBroadcast++;
        break;
        case FDB_MT_PUBLISH:
            stat_data.mPublish++;
        break;
        default:
        break;
    }
}

bool CBaseSession::sendMessageSync(CBaseJob::Ptr &ref, CFdbMessage *msg)
{
    msg->sn(mPendingMsgTable.allocateEntityId());
    if (sendMessage(msg))
    {
        msg->replaceBuffer(0); // free buffer to save memory
        msg->clearLogData();
        mPendingMsgTable.insertEntry(msg->sn(), ref);
        return true;
    }
    else
    {
        msg->setStatusMsg(FDB_ST_UNABLE_TO_SEND, "Fail when sending message!");
        if (!msg->sync())
        {
            mContainer->owner()->doReply(ref);
        }
        return false;
    }
}

void CBaseSession::terminateMessage(CBaseJob::Ptr &job, int32_t status, const char *reason)
{
    auto msg = castToMessage<CFdbMessage *>(job);
    if (msg)
    {
        msg->setStatusMsg(status, reason);
        if (!msg->sync())
        {
            auto object = mContainer->owner();
            switch (msg->type())
            {
                case FDB_MT_REQUEST:
                    object->doReply(job);
                break;
                case FDB_MT_SIDEBAND_REQUEST:
                    object->onSidebandReply((job));
                break;
                case FDB_MT_GET_EVENT:
                    object->doReturnEvent((job));
                break;
                case FDB_MT_SET_EVENT:
                default:
                    object->doStatus((job));
                break;
            }
        }
        msg->terminate(job);
    }
}

void CBaseSession::terminateMessage(FdbMsgSn_t msg_sn, int32_t status, const char *reason)
{
    bool found;
    PendingMsgTable_t::EntryContainer_t::iterator it;
    CBaseJob::Ptr &job = mPendingMsgTable.retrieveEntry(msg_sn, it, found);
    if (found)
    {
        terminateMessage(job, status, reason);
        mPendingMsgTable.deleteEntry(it);
    }
}

CFdbMessage *CBaseSession::peepPendingMessage(FdbMsgSn_t sn)
{
    bool found;

    PendingMsgTable_t::EntryContainer_t::iterator it;
    CBaseJob::Ptr &job = mPendingMsgTable.retrieveEntry(sn, it, found);
    return found ? castToMessage<CFdbMessage *>(job) : 0;
}

void CBaseSession::doResponse(CFdbMessageHeader &head)
{
    bool found;
    PendingMsgTable_t::EntryContainer_t::iterator it;
    CBaseJob::Ptr &msg_ref = mPendingMsgTable.retrieveEntry(head.serial_number(), it, found);
    if (found)
    {
        auto msg = castToMessage<CFdbMessage *>(msg_ref);
        auto object_id = head.object_id();
        if (msg->objectId() != object_id)
        {
            LOG_E("CFdbSession: object id of response %d does not match that in request: %d\n",
                    object_id, msg->objectId());
            terminateMessage(msg_ref, FDB_ST_OBJECT_NOT_FOUND, "Object ID does not match.");
            mPendingMsgTable.deleteEntry(it);
            delete[] mPayloadBuffer;
            mPayloadBuffer = 0;
            return;
        }

        auto object = mContainer->owner()->getObject(msg, false);
        if (object)
        {
            msg->update(head, mMsgPrefix);
            msg->decodeDebugInfo(head);
            msg->replaceBuffer(mPayloadBuffer, head.payload_size(), mMsgPrefix.mHeadLength);
            mPayloadBuffer = 0;
            auto type = msg->type();
            doStatistics(type, head.flag(), mStatistics.mRx);
            if (!msg->sync())
            {
                switch (type)
                {
                    case FDB_MT_REQUEST:
                        object->doReply(msg_ref);
                    break;
                    case FDB_MT_SIDEBAND_REQUEST:
                        object->onSidebandReply(msg_ref);
                    break;
                    case FDB_MT_GET_EVENT:
                        object->doReturnEvent(msg_ref);
                    break;
                    case FDB_MT_SET_EVENT:
                    default:
                        if (head.type() == FDB_MT_STATUS)
                        {
                            object->doStatus(msg_ref);
                        }
                        else
                        {
                            LOG_E("CFdbSession: request type %d doesn't match response type %d!\n",
                                  type, head.type());
                        }
                    break;
                }
            }
        }

        msg_ref->terminate(msg_ref);
        mPendingMsgTable.deleteEntry(it);
    }
}

void CBaseSession::doBroadcast(CFdbMessageHeader &head)
{
    CFdbMessage *msg = 0;
    if (head.flag() & MSG_FLAG_INITIAL_RESPONSE)
    {
        bool found;
        PendingMsgTable_t::EntryContainer_t::iterator it;
        CBaseJob::Ptr &msg_ref = mPendingMsgTable.retrieveEntry(head.serial_number(), it, found);
        if (found)
        {
            auto outgoing_msg = castToMessage<CFdbMessage *>(msg_ref);
            msg = outgoing_msg->clone(head, this);
        }
    }

    if (!msg)
    {
        msg = new CFdbMessage(head, this);
    }
    auto object = mContainer->owner()->getObject(msg, false);
    CBaseJob::Ptr msg_ref(msg);
    if (object)
    {
        msg->decodeDebugInfo(head);
        object->doBroadcast(msg_ref);
    }
}

void CBaseSession::getSessionInfo(CFdbSessionInfo &info)
{
        container()->getSocketInfo(info.mContainerSocket);
        info.mCred = &mSocket->getPeerCredentials();
        info.mConn = &mSocket->getConnectionInfo();
}

int32_t CBaseSession::checkSecurityLevel()
{
    return mContainer->checkSecurityLevel();
}
}
}
