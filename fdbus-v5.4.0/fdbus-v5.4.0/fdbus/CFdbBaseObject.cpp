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

#include <fdbus/CFdbBaseObject.h>
#include <fdbus/CBaseEndpoint.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CBaseWorker.h>
#include <fdbus/CFdbSession.h>
#include <fdbus/CFdbContext.h>
#include <utils/CFdbIfMessageHeader.h>
#include <fdbus/CFdbIfNameServer.h>
#include "CFdbWatchdog.h"
#include <utils/Log.h>
#include <string.h>

using namespace std::placeholders;

namespace ipc {
namespace fdbus {

#define FDB_MSG_TYPE_AFC_SUBSCRIBE (FDB_MSG_TYPE_SYSTEM - 1)
#define FDB_MSG_TYPE_AFC_INVOKE (FDB_MSG_TYPE_SYSTEM - 2)

class CAFCSubscribeMsg : public CFdbMessage
{
public:
    CAFCSubscribeMsg(tRegistryHandleTbl *reg_handle)
        : CFdbMessage()
    {
        if (reg_handle)
        {
            mRegHandle = *reg_handle;
        }
    }
    CAFCSubscribeMsg(CFdbMessageHeader &head
                     , CBaseSession *session
                     , tRegistryHandleTbl &reg_handle)
        : CFdbMessage(head, session)
        , mRegHandle(reg_handle)
    {
    }
    FdbMessageType_t getTypeId()
    {
        return FDB_MSG_TYPE_AFC_SUBSCRIBE;
    }

    tRegistryHandleTbl mRegHandle;
protected:
    CFdbMessage *clone(CFdbMessageHeader &head
                      , CBaseSession *session)
    {
        return new CAFCSubscribeMsg(head, session, mRegHandle);
    }
};

class CAFCInvokeMsg : public CFdbMessage
{
public:
    CAFCInvokeMsg(FdbMsgCode_t code, CFdbBaseObject::tInvokeCallbackFn &reply_callback,
                  CBaseWorker *worker, EFdbQOS qos)
        : CFdbMessage(code, qos)
        , mReplyCallback(reply_callback)
        , mWorker(worker)
    {
    }
    FdbMessageType_t getTypeId()
    {
        return FDB_MSG_TYPE_AFC_INVOKE;
    }
    CFdbBaseObject::tInvokeCallbackFn mReplyCallback;
    CBaseWorker *mWorker;
};

CFdbBaseObject::CFdbBaseObject(const char *name, CBaseWorker *worker, CFdbBaseContext *dummy,
                               EFdbEndpointRole role)
    : mEndpoint(0)
    , mFlag(FDB_OBJ_ENABLE_LOG)
    , mWatchdog(0)
    , mOnlineChannelType(FDB_SEC_NO_CHECK)
    , mWorker(worker)
    , mObjId(FDB_INVALID_ID)
    , mRole(role)
    , mRegIdAllocator(0)
{
    if (name)
    {
        mName = name;
    }
}

CFdbBaseObject::~CFdbBaseObject()
{
    if (mWatchdog)
    {
        delete mWatchdog;
    }
}

bool CFdbBaseObject::invoke(FdbSessionId_t receiver
                          , FdbMsgCode_t code
                          , IFdbMsgBuilder &data
                          , int32_t timeout
                          , EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, receiver, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    return msg->invoke(timeout);
}

bool CFdbBaseObject::invoke(FdbSessionId_t receiver
                            , CFdbMessage *msg
                            , IFdbMsgBuilder &data
                            , int32_t timeout)
{
    msg->setDestination(this, receiver);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    msg->enableTimeStamp(timeStampEnabled());
    return msg->invoke(timeout);
}

bool CFdbBaseObject::invoke(FdbMsgCode_t code
                            , IFdbMsgBuilder &data
                            , int32_t timeout
                            , EFdbQOS qos)
{
    return invoke(FDB_INVALID_ID, code, data, timeout, qos);
}

bool CFdbBaseObject::invoke(CFdbMessage *msg
                            , IFdbMsgBuilder &data
                            , int32_t timeout)
{
    return invoke(FDB_INVALID_ID, msg, data, timeout);
}

bool CFdbBaseObject::invoke(FdbSessionId_t receiver
                            , CBaseJob::Ptr &msg_ref
                            , IFdbMsgBuilder &data
                            , int32_t timeout)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    msg->setDestination(this, receiver);
    if (!msg->serialize(data, this))
    {
       return false;
    }
    msg->enableTimeStamp(timeStampEnabled());
    return msg->invoke(msg_ref, timeout);
}

bool CFdbBaseObject::invoke(CBaseJob::Ptr &msg_ref
                            , IFdbMsgBuilder &data
                            , int32_t timeout)
{
    return invoke(FDB_INVALID_ID, msg_ref, data, timeout);
}

bool CFdbBaseObject::send(FdbSessionId_t receiver
                          , FdbMsgCode_t code
                          , IFdbMsgBuilder &data
                          , EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, receiver, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    return msg->send();
}

bool CFdbBaseObject::send(FdbMsgCode_t code, IFdbMsgBuilder &data, EFdbQOS qos)
{
    return send(FDB_INVALID_ID, code, data, qos);
}

bool CFdbBaseObject::set(FdbMsgCode_t code, IFdbMsgBuilder &data, EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, FDB_INVALID_ID, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    return msg->set();
}

bool CFdbBaseObject::send(FdbSessionId_t receiver
                         , FdbMsgCode_t code
                         , const void *buffer
                         , int32_t size
                         , EFdbQOS qos
                         , const char *log_data)
{
    auto msg = new CBaseMessage(code, this, receiver, qos);
    msg->setLogData(log_data);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->send();
}

bool CFdbBaseObject::send(FdbMsgCode_t code
                         , const void *buffer
                         , int32_t size
                         , EFdbQOS qos
                         , const char *log_data)
{
    return send(FDB_INVALID_ID, code, buffer, size, qos, log_data);
}

bool CFdbBaseObject::set(FdbMsgCode_t code
                         , const void *buffer
                         , int32_t size
                         , EFdbQOS qos
                         , const char *log_data)
{
    auto msg = new CBaseMessage(code, this, FDB_INVALID_ID, qos);
    msg->setLogData(log_data);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->set();
}

bool CFdbBaseObject::publish(FdbMsgCode_t code, IFdbMsgBuilder &data, const char *topic,
                             bool force_update, EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, topic, FDB_INVALID_ID, FDB_INVALID_ID, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    msg->forceUpdate(force_update);
    return (mRole == FDB_OBJECT_ROLE_CLIENT) ? msg->publish() : msg->broadcast();
}
 
bool CFdbBaseObject::publish(FdbMsgCode_t code, const void *buffer, int32_t size, const char *topic,
                             bool force_update, EFdbQOS qos, const char *log_data)
{
    auto msg = new CBaseMessage(code, this, topic, FDB_INVALID_ID, FDB_INVALID_ID, qos);
    msg->setLogData(log_data);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    msg->forceUpdate(force_update);
    return (mRole == FDB_OBJECT_ROLE_CLIENT) ? msg->publish() : msg->broadcast();
}

bool CFdbBaseObject::publishNoQueue(FdbMsgCode_t code, const char *topic, const void *buffer, int32_t size,
                                    const char *log_data, bool force_update, EFdbQOS qos)
{
    CBaseMessage msg(code, this, FDB_INVALID_ID, qos);
    msg.expectReply(false);
    msg.forceUpdate(force_update);
    msg.setLogData(log_data);
    if (!msg.serialize(buffer, size, this))
    {
        return false;
    }
    if (topic)
    {
        msg.topic(topic);
    }
    msg.type(FDB_MT_PUBLISH);

    auto session = mEndpoint->preferredPeer(qos);
    if (session)
    {
        return session->sendMessage(&msg);
    }
    return false;
}

bool CFdbBaseObject::publishNoQueue(FdbMsgCode_t code, const char *topic, const void *buffer,
                                 int32_t size, CFdbSession *session)
{
    CBaseMessage msg(code, this);
    if (topic)
    {
        msg.topic(topic);
    }
    msg.expectReply(false);
    if (!msg.serialize(buffer, size, this))
    {
        return false;
    }
    msg.type(FDB_MT_PUBLISH);

    return session->sendMessage(&msg);
}

bool CFdbBaseObject::publishNoQueue(CFdbMessage *msg)
{
    msg->setDestination(this);
    msg->expectReply(false);
    bool ret = true;
    if (mRole == FDB_OBJECT_ROLE_CLIENT)
    {
        msg->type(FDB_MT_PUBLISH);
        auto session = mEndpoint->preferredPeer(msg->qos());
        ret = session ? session->sendMessage(msg) : false;
    }
    else
    {
        msg->type(FDB_MT_BROADCAST);
        ret = broadcast(msg);
    }
    return ret;
}

bool CFdbBaseObject::setNoQueue(CFdbMessage *msg)
{
    msg->setDestination(this);
    msg->expectReply(false);
    msg->type(FDB_MT_SET_EVENT);
    auto session = mEndpoint->preferredPeer(msg->qos());
    return session ? session->sendMessage(msg) : false;
}

void CFdbBaseObject::publishCachedEvents(CFdbSession *session)
{
    for (auto it_events = mEventCache.mCacheData.begin();
            it_events != mEventCache.mCacheData.end(); ++it_events)
    {
        auto event_code = it_events->first;
        auto &events = it_events->second;
        for (auto it_data = events.begin(); it_data != events.end(); ++it_data)
        {
            auto &topic = it_data->first;
            auto &data = it_data->second;
            publishNoQueue(event_code, topic.c_str(), data.mBuffer, data.mSize, session);
        }
    }
}

bool CFdbBaseObject::get(FdbMsgCode_t code
                         , const char *topic
                         , int32_t timeout
                         , EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, FDB_INVALID_ID, qos);
    if (!msg->serialize(0, 0, this))
    {
        delete msg;
        return false;
    }
    msg->topic(topic);
    return msg->get(timeout);
}

bool CFdbBaseObject::get(CFdbMessage *msg, const char *topic, int32_t timeout)
{
    msg->setDestination(this);
    if (!msg->serialize(0, 0, this))
    {
        delete msg;
        return false;
    }
    msg->topic(topic);
    msg->enableTimeStamp(timeStampEnabled());
    return msg->get(timeout);
}

bool CFdbBaseObject::get(CBaseJob::Ptr &msg_ref, const char *topic, int32_t timeout)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    msg->setDestination(this);
    if (!msg->serialize(0, 0, this))
    {
        return false;
    }
    msg->topic(topic);
    msg->enableTimeStamp(timeStampEnabled());
    return msg->get(msg_ref, timeout);
}

bool CFdbBaseObject::broadcast(FdbMsgCode_t code
                               , IFdbMsgBuilder &data
                               , const char *topic
                               , EFdbQOS qos)
{
    auto msg = new CFdbMessage(code, this, topic, FDB_INVALID_ID, FDB_INVALID_ID, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    return msg->broadcast();
}

bool CFdbBaseObject::broadcast(FdbMsgCode_t code
                              , const void *buffer
                              , int32_t size
                              , const char *topic
                              , EFdbQOS qos
                              , const char *log_data)
{
    auto msg = new CFdbMessage(code, this, topic, FDB_INVALID_ID, FDB_INVALID_ID, qos);
    msg->setLogData(log_data);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->broadcast();
}

void CFdbBaseObject::broadcastLogNoQueue(FdbMsgCode_t code, const uint8_t *data, int32_t size,
                                         const char *topic, EFdbQOS qos)
{
    CFdbMessage msg(code, this, topic, FDB_INVALID_ID, FDB_INVALID_ID, qos);
    if (!msg.serialize(data, size, this))
    {
        return;
    }
    msg.enableLog(false);

    broadcast(&msg);
}

bool CFdbBaseObject::unsubscribe(CFdbMsgSubscribeList &msg_list, EFdbQOS qos)
{
    auto msg = new CBaseMessage(FDB_INVALID_ID, this, FDB_INVALID_ID, qos);
    msg->type(FDB_MT_SUBSCRIBE_REQ);
    CFdbParcelableBuilder builder(msg_list);
    if (!msg->serialize(builder, this))
    {
        delete msg;
        return false;
    }
    return msg->unsubscribe();
}

bool CFdbBaseObject::unsubscribe(EFdbQOS qos)
{
    CFdbMsgSubscribeList msg_list;
    return unsubscribe(msg_list, qos);
}

void CFdbBaseObject::migrateToWorker(CBaseJob::Ptr &msg_ref, tRemoteCallback callback, CBaseWorker *worker)
{
    if (!worker)
    {
        worker = mWorker;
    }
    if (worker)
    {
        if (enableMigrate())
        {
            auto msg = castToMessage<CBaseMessage *>(msg_ref);
            msg->setCallable(std::bind(callback, this, _1));
            if (!worker->sendAsyncSwap(msg_ref))
            {
            }
        }
    }
    else
    {
        try
        {
            (this->*callback)(msg_ref);
        }
        catch (...)
        {
            LOG_E("CFdbBaseObject: except is caught at migrateToWorker for service %s!\n", mName.c_str());
        }
    }
}

class COnOnlineJob : public CMethodJob<CFdbBaseObject>
{
public:
    COnOnlineJob(CFdbBaseObject *object, const CFdbOnlineInfo &info, bool online)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callOnOnline, JOB_FORCE_RUN)
        , mInfo(info)
        , mOnline(online)
    {}

    CFdbOnlineInfo mInfo;
    bool mOnline;
};

void CFdbBaseObject::callOnOnline(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<COnOnlineJob *>(job);
    if (the_job)
    {
        callOnline(the_job->mInfo, the_job->mOnline);
    }
}

void CFdbBaseObject::migrateToWorker(const CFdbOnlineInfo &info, bool online, CBaseWorker *worker)
{
    if (!worker)
    {
        worker = mWorker;
    }
    if (worker)
    {
        if (enableMigrate())
        {
            worker->sendAsync(new COnOnlineJob(this, info, online));
        }
    }
    else
    {
        callOnline(info, online);
    }
}

void CFdbBaseObject::callSubscribe(CBaseJob::Ptr &msg_ref)
{
    try // catch exception to avoid missing of auto-reply
    {
        onSubscribe(msg_ref);
    }
    catch (...)
    {
        LOG_E("CFdbBaseObject: except is caught at callSubscribe for service %s!\n", mName.c_str());
    }
    CFdbMessage::autoReply(msg_ref, FDB_ST_AUTO_REPLY_OK,
                            "Automatically reply to subscribe request.");
}

void CFdbBaseObject::callBroadcast(CBaseJob::Ptr &msg_ref)
{
    onBroadcast(msg_ref);
}

void CFdbBaseObject::callInvoke(CBaseJob::Ptr &msg_ref)
{
    try // catch exception to avoid missing of auto-reply
    {
        onInvoke(msg_ref);
    }
    catch (...)
    {
        LOG_E("CFdbBaseObject: except is caught at callInvoke for service %s!\n", mName.c_str());
    }
    CFdbMessage::autoReply(msg_ref, FDB_ST_AUTO_REPLY_OK, "Automatically reply to request.");
}

void CFdbBaseObject::callSetEvent(CBaseJob::Ptr &msg_ref)
{
    try // catch exception to avoid missing of auto-reply
    {
        onSetEvent(msg_ref);
    }
    catch (...)
    {
        LOG_E("CFdbBaseObject: except is caught at callSetEvent for service %s!\n", mName.c_str());
    }
    CFdbMessage::autoReply(msg_ref, FDB_ST_AUTO_REPLY_OK, "Automatically reply to request.");
}

void CFdbBaseObject::callOnline(const CFdbOnlineInfo &info, bool online)
{
    try
    {
        if (online)
        {
            onOnline(info);
        }
        else
        {
            onOffline(info);
        }
    }
    catch (...)
    {
        LOG_E("CFdbBaseObject: except is caught at callOnline for service %s!\n", mName.c_str());
    }
}

void CFdbBaseObject::notifyOnline(CFdbSession *session, bool is_first)
{
    if (isPrimary())
    {
        mEndpoint->updateSessionInfo(session);
    }
    if (mWatchdog)
    {
        mWatchdog->addDog(session);
    }
    else if (mFlag & FDB_OBJ_ENABLE_WATCHDOG)
    {
        createWatchdog();
    }
    CFdbOnlineInfo info = {session->sid(),
                           is_first,
                           session->qos()};
    if (mEndpoint->checkOnlineChannel(mOnlineChannelType, info))
    {   // for client, only notify online/offline of secure channel
        // if secure channel is enabled;
        // for server, online/offline of all channels are notified;
        // but the is_first/is_last takes all channel into account
        LOG_I("CFdbBaseObject - server %s online: sid %d, QOS: %d, is first: %d.\n",
                mName.c_str(), info.mSid, info.mQOS, info.mFirstOrLast);

        tEvtHandleTbl events;
        mEventDispather.dumpEvents(events);
        subscribeEvents(events, 0);

        migrateToWorker(info, true);
    }
}

void CFdbBaseObject::notifyOffline(CFdbSession *session, bool is_last)
{
    if (mWatchdog)
    {
        mWatchdog->removeDog(session);
    }
    CFdbOnlineInfo info = {session->sid(),
                           is_last,
                           session->qos()};
    if ((mEndpoint->checkOnlineChannel(mOnlineChannelType, info)))
    {   // for client, only notify online/offline of secure channel
        // if secure channel is enabled;
        // for server, online/offline of all channels are notified;
        // but the is_first/is_last takes all channel into account
        LOG_I("server %s offline: sid %d, QOS %d, is last: %d.\n",
                mName.c_str(), info.mSid, info.mQOS, info.mFirstOrLast);
        migrateToWorker(info, false);
    }
}

void CFdbBaseObject::callReply(CBaseJob::Ptr &msg_ref)
{
    onReply(msg_ref);
}

void CFdbBaseObject::callReturnEvent(CBaseJob::Ptr &msg_ref)
{
    onGetEvent(msg_ref);
}

void CFdbBaseObject::callStatus(CBaseJob::Ptr &msg_ref)
{
    auto fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    int32_t error_code;
    std::string description;
    if (!fdb_msg->decodeStatus(error_code, description))
    {
        return;
    }

    onStatus(msg_ref, error_code, description.c_str());
}

const CFdbBaseObject::CEventData *CFdbBaseObject::getCachedEventData(FdbMsgCode_t msg_code,
                                                                     const char *topic)
{
    return mEventCache.get(msg_code, topic);
}

void CFdbBaseObject::broadcastCached(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    auto session = msg->getSession();
    if (!session)
    {
        return;
    }
    const CFdbMsgSubscribeItem *sub_item;
    /* iterate all message id subscribed */
    FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
    {
        FdbMsgCode_t msg_code = sub_item->msg_code();
        const char *topic = "";
        if (sub_item->has_topic())
        {
            topic = sub_item->topic().c_str();
        }
        if (topic[0] == '\0')
        {
            auto it_topics = mEventCache.mCacheData.find(msg_code);
            if (it_topics != mEventCache.mCacheData.end())
            {
                auto &topics = it_topics->second;
                for (auto it_data = topics.begin(); it_data != topics.end(); ++it_data)
                {
                    auto &cached_topic = it_data->first;
                    auto &cached_data = it_data->second;
                    CFdbMessage broadcast_msg(msg_code, msg, cached_topic.c_str());
                    if (broadcast_msg.serialize(cached_data.mBuffer, cached_data.mSize, this))
                    {
                        broadcast_msg.forceUpdate(true);
                        broadcast(&broadcast_msg, session);
                    }
                }
            }
        }
        else
        {
            auto cached_data = getCachedEventData(msg_code, topic);
            if (cached_data)
            {
                CFdbMessage broadcast_msg(msg_code, msg, topic);
                if (broadcast_msg.serialize(cached_data->mBuffer, cached_data->mSize, this))
                {
                    broadcast_msg.forceUpdate(true);
                    broadcast(&broadcast_msg, session);
                }
            }
        }
    }
    FDB_END_FOREACH_SIGNAL()
}

void CFdbBaseObject::doSubscribe(CBaseJob::Ptr &msg_ref)
{
    if (mFlag & FDB_OBJ_ENABLE_EVENT_CACHE)
    {
        try // catch exception to avoid missing of auto-reply
        {
            /* broadcast current value of event/topic pair */
            broadcastCached(msg_ref);
        }
        catch (...)
        {
            LOG_E("CFdbBaseObject: except is caught at doSubscribe for service %s!\n", mName.c_str());
        }

        CFdbMessage::autoReply(msg_ref, FDB_ST_AUTO_REPLY_OK, "Automatically reply to subscribe request.");
        return;
    }

    migrateToWorker(msg_ref, &CFdbBaseObject::callSubscribe);
}

void CFdbBaseObject::doBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (updateEventCache(msg))
    {
        if (mEventDispather.enabled())
        {
            callBroadcast(msg_ref);
        }
        else
        {
            migrateToWorker(msg_ref, &CFdbBaseObject::callBroadcast);
        }
    }
}

void CFdbBaseObject::doGetEvent(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CBaseMessage *>(msg_ref);
    if (mFlag & FDB_OBJ_ENABLE_EVENT_CACHE)
    {
        auto cached_data = getCachedEventData(msg->code(), msg->topic().c_str());
        if (cached_data)
        {
            msg->replyEventCache(msg_ref, cached_data->mBuffer, cached_data->mSize);
        }
        else
        {
            msg->statusf(msg_ref, FDB_ST_NON_EXIST, "Event %d topic %s doesn't exists!",
                         msg->code(), msg->topic().c_str());
        }
    }
    else
    {
        msg->status(msg_ref, FDB_ST_NON_EXIST, "Event cache is not enabled!");
    }
}

void CFdbBaseObject::doSetEvent(CBaseJob::Ptr &msg_ref)
{
    if (mSetEvtDispather.enabled())
    {
        callSetEvent(msg_ref);
    }
    else
    {
        migrateToWorker(msg_ref, &CFdbBaseObject::callSetEvent);
    }
}

void CFdbBaseObject::doInvoke(CBaseJob::Ptr &msg_ref)
{
    if (mMsgDispather.enabled())
    {
        callInvoke(msg_ref);
    }
    else
    {
        migrateToWorker(msg_ref, &CFdbBaseObject::callInvoke);
    }
}

void CFdbBaseObject::doReply(CBaseJob::Ptr &msg_ref)
{
    auto *msg = castToMessage<CBaseMessage *>(msg_ref);
    if (msg->getTypeId() == FDB_MSG_TYPE_AFC_INVOKE)
    {
        auto afc_msg = castToMessage<CAFCInvokeMsg *>(msg_ref);
        fdbMigrateCallback(msg_ref, msg, afc_msg->mReplyCallback, afc_msg->mWorker ? afc_msg->mWorker : mWorker, this);
    }
    else
    {
        migrateToWorker(msg_ref, &CFdbBaseObject::callReply);
    }
}

void CFdbBaseObject::doReturnEvent(CBaseJob::Ptr &msg_ref)
{
    migrateToWorker(msg_ref, &CFdbBaseObject::callReturnEvent);
}

void CFdbBaseObject::doStatus(CBaseJob::Ptr &msg_ref)
{
    migrateToWorker(msg_ref, &CFdbBaseObject::callStatus);
}

void CFdbBaseObject::doPublish(CBaseJob::Ptr &msg_ref)
{
    bool published = false;
    auto msg = castToMessage<CBaseMessage *>(msg_ref);

    auto action = updateEventCache(msg);
    if (action == 1)
    {
        CFdbMessage out_msg(msg->code(), this, msg->topic().c_str(),
                            msg->session(), FDB_INVALID_ID, msg->qos());
        if (out_msg.serialize(msg->getPayloadBuffer(), msg->getPayloadSize(), this))
        {
            out_msg.forceUpdate(msg->isForceUpdate());
            out_msg.setNoReflect(true);
            published = broadcastMsg(&out_msg);
        }

        onPublish(msg_ref);

        // check if auto-reply is required
        CFdbMessage::autoReply(msg_ref, FDB_ST_AUTO_REPLY_OK, "Automatically reply to publish request.");
    }

    if (action && !published)
    {
        // The publish action is not allowed to forward the event to subscribers, which means
        // the framework is not able to handle the publishment. Thereby, we give a chance to
        // the user to handle it.
        msg->type(FDB_MT_SET_EVENT);
        doSetEvent(msg_ref);
    }
}

bool CFdbBaseObject::invoke(FdbSessionId_t receiver
                           , FdbMsgCode_t code
                           , const void *buffer
                           , int32_t size
                           , int32_t timeout
                           , EFdbQOS qos
                           , const char *log_data)
{
    auto msg = new CBaseMessage(code, this, receiver, qos);
    msg->setLogData(log_data);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->invoke(timeout);
}

bool CFdbBaseObject::invoke(FdbMsgCode_t code
                           , const void *buffer
                           , int32_t size
                           , int32_t timeout
                           , EFdbQOS qos
                           , const char *log_data)
{
    return invoke(FDB_INVALID_ID, code, buffer, size, timeout, qos, log_data);
}


bool CFdbBaseObject::invoke(FdbSessionId_t receiver
                           , CFdbMessage *msg
                           , const void *buffer
                           , int32_t size
                           , int32_t timeout)
{
    msg->setDestination(this, receiver);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    msg->enableTimeStamp(timeStampEnabled());
    return msg->invoke(timeout);
}

bool CFdbBaseObject::invoke(CFdbMessage *msg
                           , const void *buffer
                           , int32_t size
                           , int32_t timeout)
{
    return invoke(FDB_INVALID_ID, msg, buffer, size, timeout);
}

bool CFdbBaseObject::invoke(FdbSessionId_t receiver
                           , CBaseJob::Ptr &msg_ref
                           , const void *buffer
                           , int32_t size
                           , int32_t timeout)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    msg->setDestination(this, receiver);
    if (!msg->serialize(buffer, size, this))
    {
       return false;
    }
    msg->enableTimeStamp(timeStampEnabled());
    return msg->invoke(msg_ref, timeout);
}

bool CFdbBaseObject::invoke(CBaseJob::Ptr &msg_ref
                           , const void *buffer
                           , int32_t size
                           , int32_t timeout)
{
    return invoke(FDB_INVALID_ID, msg_ref, buffer, size, timeout);
}

bool CFdbBaseObject::subscribe(CFdbMsgSubscribeList &msg_list
                              , int32_t timeout
                              , EFdbQOS qos)
{
    auto msg = new CBaseMessage(FDB_INVALID_ID, this, FDB_INVALID_ID, qos);
    msg->type(FDB_MT_SUBSCRIBE_REQ);
    CFdbParcelableBuilder builder(msg_list);
    if (!msg->serialize(builder, this))
    {
        delete msg;
        return false;
    }
    return msg->subscribe(timeout);
}

bool CFdbBaseObject::subscribe(CFdbMsgSubscribeList &msg_list
                              , CFdbMessage *msg
                              , int32_t timeout)
{
    msg->type(FDB_MT_SUBSCRIBE_REQ);
    msg->setDestination(this);
    CFdbParcelableBuilder builder(msg_list);
    if (!msg->serialize(builder, this))
    {
        delete msg;
        return false;
    }
    msg->enableTimeStamp(timeStampEnabled());
    return msg->subscribe(timeout);
}

bool CFdbBaseObject::subscribeSync(CFdbMsgSubscribeList &msg_list
                                  , int32_t timeout
                                  , EFdbQOS qos)
{
    auto msg = new CBaseMessage(FDB_INVALID_ID, this, FDB_INVALID_ID, qos);
    msg->type(FDB_MT_SUBSCRIBE_REQ);
    msg->setDestination(this);
    CFdbParcelableBuilder builder(msg_list);
    if (!msg->serialize(builder, this))
    {
        delete msg;
        return false;
    }
    CBaseJob::Ptr msg_ref(msg);
    if (!msg->subscribe(msg_ref, timeout))
    {
        return false;
    }
    if (msg->isError())
    {
        return false;
    }
    if (worker())
    {
        worker()->flush();
    }

    return true;
}

void CFdbBaseObject::addNotifyItem(CFdbMsgSubscribeList &msg_list
                                  , FdbMsgCode_t msg_code
                                  , const char *topic
                                  , uint32_t flag)
{
    msg_list.addNotifyItem(msg_code, topic, flag);
}

void CFdbBaseObject::addNotifyGroup(CFdbMsgSubscribeList &msg_list
                                    , FdbEventGroup_t event_group
                                    , const char *topic
                                    , uint32_t flag)
{
    msg_list.addNotifyGroup(event_group, topic);
}

void CFdbBaseObject::addTriggerItem(CFdbMsgTriggerList &msg_list
                                     , FdbMsgCode_t msg_code
                                     , const char *topic)
{
    msg_list.addTriggerItem(msg_code, topic);
}

 void CFdbBaseObject::addTriggerGroup(CFdbMsgTriggerList &msg_list
                                      , FdbEventGroup_t event_group
                                      , const char *topic)
{
    msg_list.addTriggerGroup(event_group, topic);
}

void CFdbBaseObject::subscribe(CFdbSession *session,
                               FdbMsgCode_t msg,
                               FdbObjectId_t obj_id,
                               const char *topic,
                               uint32_t flag)
{
    CEventSubscribeHandle &subscribe_handle = fdbIsGroup(msg) ?
                                              mGroupSubscribeHandle : mEventSubscribeHandle;
    subscribe_handle.subscribe(session, msg, obj_id, topic, flag);
}

void CFdbBaseObject::unsubscribe(CFdbSession *session,
                                 FdbMsgCode_t msg,
                                 FdbObjectId_t obj_id,
                                 const char *topic)
{
    CEventSubscribeHandle &subscribe_handle = fdbIsGroup(msg) ?
                                              mGroupSubscribeHandle : mEventSubscribeHandle;
    subscribe_handle.unsubscribe(session, msg, obj_id, topic);
}

void CFdbBaseObject::unsubscribe(CFdbSession *session)
{
    mGroupSubscribeHandle.unsubscribe(session);
    mEventSubscribeHandle.unsubscribe(session);
}

void CFdbBaseObject::unsubscribe(FdbObjectId_t obj_id)
{
    mGroupSubscribeHandle.unsubscribe(obj_id);
    mEventSubscribeHandle.unsubscribe(obj_id);
}

bool CFdbBaseObject::CEventCache::add(FdbMsgCode_t event,
                                      const char *topic,
                                      IFdbMsgBuilder &data,
                                      bool allow_event_route,
                                      bool allow_event_cache)
{
    if (!topic)
    {
        topic = "";
    }
    auto &cached_event = mCacheData[event][topic];
    cached_event.mAllowRoute = allow_event_route;
    cached_event.mAllowCache = allow_event_cache;
    int32_t size = data.build();
    if (size < 0)
    {
        return false;
    }
    cached_event.setEventCache(0, size);
    data.toBuffer(cached_event.mBuffer, size);
    return true;
}

bool CFdbBaseObject::CEventCache::add(FdbMsgCode_t event,
                                      const char *topic,
                                      const void *buffer,
                                      int32_t size,
                                      bool allow_event_route,
                                      bool allow_event_cache)
{
    if (!topic)
    {
        topic = "";
    }
    auto &cached_event = mCacheData[event][topic];
    cached_event.mAllowRoute = allow_event_route;
    cached_event.mAllowCache = allow_event_cache;
    cached_event.setEventCache((const uint8_t *)buffer, size);
    return true;
}

void CFdbBaseObject::CEventCache::replace(FdbMsgCode_t id,
                                          const char *topic,
                                          const uint8_t *buffer,
                                          int32_t size,
                                          bool allow_event_route,
                                          bool allow_event_cache)
{
    auto &cached_event = mCacheData[id][topic];
    if (buffer && size)
    {
        cached_event.replaceCacheData(const_cast<uint8_t *>(buffer), size);
    }
    cached_event.mAllowRoute = allow_event_route;
    cached_event.mAllowCache = allow_event_cache;
}

CFdbBaseObject::CEventData *CFdbBaseObject::CEventCache::get(
                        FdbMsgCode_t msg_code, const char *topic)
{
    auto it_code = mCacheData.find(msg_code);
    if (it_code != mCacheData.end())
    {
        auto &topics = it_code->second;
        auto it_topic = topics.find(topic);
        if (it_topic != topics.end())
        {
            auto &data = it_topic->second;
            return &data;
        }
    }
    return 0;
}

void CFdbBaseObject::CEventCache::erase(FdbMsgCode_t msg_code, const char *topic)
{
    auto it_code = mCacheData.find(msg_code);
    if (it_code != mCacheData.end())
    {
        auto &topics = it_code->second;
        auto it_topic = topics.find(topic);
        if (it_topic != topics.end())
        {
            topics.erase(it_topic);
        }
        if (topics.empty())
        {
            mCacheData.erase(it_code);
        }
    }
}

/*
 * -1: act as if cache is disabled or skipped
 *  1: act as if cache is updated
 *  0: do nothing
 *
 * for server:
 * if FDB_OBJ_ENABLE_EVENT_CACHE is set:
 *       type                 allow_event_route   !allow_event_route
 *   FDB_MT_PUBLISH                0, 1         server: -1; client: 0, 1
 *   FDB_MT_BROADCAST              0, 1                0, 1
 *
 * if FDB_OBJ_ENABLE_EVENT_CACHE is not set:
 *       type          FDB_OBJ_ENABLE_EVENT_ROUTE   !FDB_OBJ_ENABLE_EVENT_ROUTE
 *   FDB_MT_PUBLISH                 1                  -1
 *   FDB_MT_BROADCAST              -1                  -1
 */
int32_t CFdbBaseObject::CEventCache::update(CFdbMessage *msg, bool allow_event_route,
                                            EFdbEndpointRole role)
{
    // update cached event data
    CEventData *cached_event = 0;
    auto it_topics = mCacheData.find(msg->code());
    if (it_topics != mCacheData.end())
    {
        auto it_data = it_topics->second.find(msg->topic());
        if (it_data != it_topics->second.end())
        {
            cached_event = &it_data->second;
            if (!cached_event->mAllowCache)
            {
                return -1;
            }
            allow_event_route = cached_event->mAllowRoute;
        }
    }


    if ((role != FDB_OBJECT_ROLE_CLIENT) &&
            ((msg->type() == FDB_MT_PUBLISH) && !allow_event_route))
    {
        return -1;
    }

    if (!cached_event)
    {
        cached_event = &mCacheData[msg->code()][msg->topic()];
        cached_event->mAllowRoute = allow_event_route;
    }

    auto updated = cached_event->setEventCache(msg->getPayloadBuffer(),
                                               msg->getPayloadSize());
    if (!updated && !msg->isForceUpdate())
    {
        return 0;
    }

    return 1;
}

int32_t CFdbBaseObject::updateEventCache(CFdbMessage *msg)
{
    int32_t action;
    if (mFlag & FDB_OBJ_ENABLE_EVENT_CACHE)
    {
        action = mEventCache.update(msg, !!(mFlag & FDB_OBJ_ENABLE_EVENT_ROUTE), mRole);
    }
    else
    {
        action = ((msg->type() == FDB_MT_PUBLISH) && !(mFlag & FDB_OBJ_ENABLE_EVENT_ROUTE)) ?
                  1 : -1;
    }
    return action;
}


bool CFdbBaseObject::broadcastMsg(CFdbMessage *msg)
{
    msg->type(FDB_MT_BROADCAST);
    auto code = msg->code();
    auto sent = mEventSubscribeHandle.broadcast(msg, code);
    sent |= mGroupSubscribeHandle.broadcast(msg, fdbMakeGroup(code));
    if (!fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
    {
        sent |= mGroupSubscribeHandle.broadcast(msg, fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS));
    }
    return sent;
}

bool CFdbBaseObject::broadcast(CFdbMessage *msg)
{
    return updateEventCache(msg) ? broadcastMsg(msg) : false;
}

bool CFdbBaseObject::broadcast(CFdbMessage *msg, CFdbSession *session)
{
    if (updateEventCache(msg))
    {
        msg->type(FDB_MT_BROADCAST);
        auto code = msg->code();
        if (mEventSubscribeHandle.broadcast(msg, session, code))
        {
            return true;
        }
        else if (mGroupSubscribeHandle.broadcast(msg, session, fdbMakeGroup(code)))
        {
            return true;
        }
        else if (fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
        {
            return false;
        }
        else
        {
            return mGroupSubscribeHandle.broadcast(msg, session, fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS));
        }
    }
    else
    {
        return true;
    }
}

bool CFdbBaseObject::publish(CFdbMessage *msg, CFdbSession *session)
{
    if (updateEventCache(msg) && session)
    {
        return session->sendMessage(msg);
    }
    return false;
}

void CFdbBaseObject::getSubscribeTable(tFdbSubscribeMsgTbl &table)
{
    mEventSubscribeHandle.getSubscribeTable(table);
    mGroupSubscribeHandle.getSubscribeTable(table);
}

void CFdbBaseObject::getSubscribeTable(FdbMsgCode_t code, tFdbFilterSets &topics)
{
    mEventSubscribeHandle.getSubscribeTable(code, topics);
    mGroupSubscribeHandle.getSubscribeTable(fdbMakeGroup(code), topics);
    if (!fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
    {
        mGroupSubscribeHandle.getSubscribeTable(fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), topics);
    }
}

void CFdbBaseObject::getSubscribeTable(FdbMsgCode_t code, CFdbSession *session,
                                        tFdbFilterSets &topic_tbl)
{
    mEventSubscribeHandle.getSubscribeTable(code, session, topic_tbl);
    mGroupSubscribeHandle.getSubscribeTable(fdbMakeGroup(code), session, topic_tbl);
    if (!fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
    {
        mGroupSubscribeHandle.getSubscribeTable(fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), session, topic_tbl);
    }
}

void CFdbBaseObject::getSubscribeTable(FdbMsgCode_t code, const char *topic,
                                       tSubscribedSessionSets &session_tbl)
{
    mEventSubscribeHandle.getSubscribeTable(code, topic, session_tbl);
    mGroupSubscribeHandle.getSubscribeTable(fdbMakeGroup(code), topic, session_tbl);
    if (!fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
    {
        mGroupSubscribeHandle.getSubscribeTable(fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), topic, session_tbl);
    }
}

FdbObjectId_t CFdbBaseObject::addToEndpoint(CBaseEndpoint *endpoint, FdbObjectId_t obj_id)
{
    mEndpoint = endpoint;
    mObjId = obj_id;
    obj_id = endpoint->addObject(this);
    return mObjId;
}

void CFdbBaseObject::removeFromEndpoint()
{
    if (mEndpoint)
    {
        mEndpoint->removeObject(this);
        mEndpoint = 0;
    }
}

FdbEndpointId_t CFdbBaseObject::epid() const
{
    return mEndpoint ? mEndpoint->epid() : FDB_INVALID_ID;
}


FdbObjectId_t CFdbBaseObject::doBind(CBaseEndpoint *endpoint, FdbObjectId_t obj_id)
{
    mRole = FDB_OBJECT_ROLE_SERVER;
    return addToEndpoint(endpoint, obj_id);
}

FdbObjectId_t CFdbBaseObject::doConnect(CBaseEndpoint *endpoint, FdbObjectId_t obj_id)
{
    mRole = FDB_OBJECT_ROLE_CLIENT;
    return addToEndpoint(endpoint, obj_id);
}

void CFdbBaseObject::doUnbind()
{
    removeFromEndpoint();
}

void CFdbBaseObject::doDisconnect()
{
    removeFromEndpoint();
}

//================================== Bind ==========================================
class CBindObjectJob : public CMethodJob<CFdbBaseObject>
{
public:
    CBindObjectJob(CFdbBaseObject *object, CBaseEndpoint *endpoint, FdbObjectId_t &oid)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callBindObject, JOB_FORCE_RUN)
        , mEndpoint(endpoint)
        , mOid(oid)
    {
    }

    CBaseEndpoint *mEndpoint;
    FdbObjectId_t &mOid;
};

void CFdbBaseObject::callBindObject(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CBindObjectJob *>(job);
    if (the_job)
    {
        the_job->mOid = doBind(the_job->mEndpoint, the_job->mOid);
    }
}

FdbObjectId_t CFdbBaseObject::bind(CBaseEndpoint *endpoint, FdbObjectId_t oid)
{
    endpoint->context()->sendSyncEndeavor(new CBindObjectJob(this, endpoint, oid), 0, true);
    return oid;
}

//================================== Connect ==========================================
class CConnectObjectJob : public CMethodJob<CFdbBaseObject>
{
public:
    CConnectObjectJob(CFdbBaseObject *object, CBaseEndpoint *endpoint, FdbObjectId_t &oid)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callConnectObject, JOB_FORCE_RUN)
        , mEndpoint(endpoint)
        , mOid(oid)
    {
    }

    CBaseEndpoint *mEndpoint;
    FdbObjectId_t &mOid;
};

void CFdbBaseObject::callConnectObject(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CConnectObjectJob *>(job);
    if (the_job)
    {
        the_job->mOid = doConnect(the_job->mEndpoint, the_job->mOid);
    }
}

FdbObjectId_t CFdbBaseObject::connect(CBaseEndpoint *endpoint, FdbObjectId_t oid)
{
    endpoint->context()->sendSyncEndeavor(new CConnectObjectJob(this, endpoint, oid));
    return oid;
}

//================================== Unbind ==========================================
class CUnbindObjectJob : public CMethodJob<CFdbBaseObject>
{
public:
    CUnbindObjectJob(CFdbBaseObject *object)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callUnbindObject, JOB_FORCE_RUN)
    {
    }
};

void CFdbBaseObject::callUnbindObject(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
     doUnbind();
}

void CFdbBaseObject::unbind()
{
    mEndpoint->context()->sendSyncEndeavor(new CUnbindObjectJob(this), 0, true);
    // From now on, there will be no jobs migrated to worker thread. Applying a
    // flush to worker thread to ensure no one refers to the object.
}

//================================== Disconnect ==========================================
class CDisconnectObjectJob : public CMethodJob<CFdbBaseObject>
{
public:
    CDisconnectObjectJob(CFdbBaseObject *object)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callDisconnectObject, JOB_FORCE_RUN)
    {
    }
};

void CFdbBaseObject::callDisconnectObject(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
     doDisconnect();
}

void CFdbBaseObject::disconnect()
{
    unsubscribe();
    mEndpoint->context()->sendSyncEndeavor(new CDisconnectObjectJob(this), 0, true);
    // From now on, there will be no jobs migrated to worker thread. Applying a
    // flush to worker thread to ensure no one refers to the object.
}

bool CFdbBaseObject::broadcast(FdbSessionId_t sid
                              , FdbMsgCode_t code
                              , IFdbMsgBuilder &data
                              , const char *topic
                              , EFdbQOS qos)
{
    auto msg = new CFdbMessage(code, this, topic, sid, FDB_INVALID_ID, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    return msg->broadcast();
}

bool CFdbBaseObject::broadcast(FdbSessionId_t sid
                      , FdbMsgCode_t code
                      , const void *buffer
                      , int32_t size
                      , const char *topic
                      , EFdbQOS qos
                      , const char *log_data)
{
    auto msg = new CFdbMessage(code, this, topic, sid, FDB_INVALID_ID, qos);
    msg->setLogData(log_data);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->broadcast();
}

void CFdbBaseObject::initEventCache(FdbMsgCode_t event,
                                    const char *topic,
                                    IFdbMsgBuilder &data,
                                    bool allow_event_route,
                                    bool allow_event_cache)
{
    mEventCache.add(event, topic, data, allow_event_route, allow_event_cache);
}
                
void CFdbBaseObject::initEventCache(FdbMsgCode_t event,
                                    const char *topic,
                                    const void *buffer,
                                    int32_t size,
                                    bool allow_event_route,
                                    bool allow_event_cache)
{
    mEventCache.add(event, topic, buffer, size, allow_event_route, allow_event_cache);
}

bool CFdbBaseObject::invokeSideband(FdbMsgCode_t code
                                  , IFdbMsgBuilder &data
                                  , int32_t timeout
                                  , EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, FDB_INVALID_ID, qos);
    if (!msg->serialize(data, this))
    {
        delete msg;
        return false;
    }
    return msg->invokeSideband(timeout);
}

bool CFdbBaseObject::invokeSideband(FdbMsgCode_t code
                                  , const void *buffer
                                  , int32_t size
                                  , int32_t timeout
                                  , EFdbQOS qos)
{
    auto msg = new CBaseMessage(code, this, FDB_INVALID_ID, qos);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->invokeSideband(timeout);
}

bool CFdbBaseObject::invokeSideband(FdbSessionId_t receiver
                                  , CFdbMessage *msg
                                  , const void *buffer
                                  , int32_t size
                                  , int32_t timeout)
{
    msg->setDestination(this, receiver);
    if (!msg->serialize(buffer, size, this))
    {
        delete msg;
        return false;
    }
    return msg->invokeSideband(timeout);
}

bool CFdbBaseObject::sendSidebandNoQueue(CFdbSession *session, CFdbMessage &msg, bool expect_reply)
{
    msg.expectReply(expect_reply);
    msg.type(FDB_MT_SIDEBAND_REQUEST);
    if (!session)
    {
        session = msg.getSession();
    }
    if (session)
    {
        msg.qos(session->qos());
        return session->sendMessage(&msg);
    }
    return false;
}

bool CFdbBaseObject::sendSideband(CFdbSession *session, FdbMsgCode_t code, IFdbMsgBuilder &data)
{
    CFdbMessage msg(code, this);
    if (!msg.serialize(data, this))
    {
        return false;
    }
    return sendSidebandNoQueue(session, msg, false);
}

bool CFdbBaseObject::sendSideband(FdbMsgCode_t code, IFdbMsgBuilder &data)
{
    return sendSideband(0, code, data);
}

bool CFdbBaseObject::sendSideband(CFdbSession *session, FdbMsgCode_t code,
                                  const void *buffer, int32_t size)
{
    CFdbMessage msg(code, this);
    if (!msg.serialize(buffer, size, this))
    {
        return false;
    }
    return sendSidebandNoQueue(session, msg, false);
}

bool CFdbBaseObject::sendSideband(FdbMsgCode_t code, const void *buffer, int32_t size)
{
    return sendSideband(0, code, buffer, size);
}

CFdbBaseObject::CEventData::CEventData()
    : mBuffer(0)
    , mSize(0)
    , mAllowRoute(false)
    , mAllowCache(true)
{
}

CFdbBaseObject::CEventData::~CEventData()
{
    if (mBuffer)
    {
        delete[] mBuffer;
        mBuffer = 0;
    }
}

bool CFdbBaseObject::CEventData::setEventCache(const uint8_t *buffer, int32_t size)
{
    if (size == mSize)
    {
        if (mBuffer)
        {
            if (buffer)
            {
                if (!memcmp(mBuffer, buffer, size))
                {
                    return false;
                }
            }
        }
        else if (size)
        {
            try
            {
                mBuffer = new uint8_t[size];
            }
            catch (...)
            {
                LOG_E("CFdbBaseObject: fail to allocate event cache with size %d\n", size);
                mBuffer = 0;
                mSize = 0;
            }
        }
    }
    else
    {
        if (mBuffer)
        {
            delete[] mBuffer;
            mBuffer = 0;
            mSize = 0;
        }

        if (size)
        {
            try
            {
                mBuffer = new uint8_t[size];
            }
            catch (...)
            {
                LOG_E("CFdbBaseObject: fail to allocate event cache with size %d\n", size);
            }
        }
        mSize = size;
    }

    if (size && buffer)
    {
        memcpy(mBuffer, buffer, size);
    }
    return true;
}

void CFdbBaseObject::CEventData::replaceCacheData(uint8_t *buffer, int32_t size)
{
    if (mBuffer)
    {
        delete[] mBuffer;
        mBuffer = 0;
    }
    mBuffer = buffer;
    mSize = size;
}

void CFdbBaseObject::onSidebandInvoke(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    switch (msg->code())
    {
        case FDB_SIDEBAND_QUERY_EVT_CACHE:
        {
            FdbMsgEventCache msg_cache;
            for (auto it_topics = mEventCache.mCacheData.begin();
                    it_topics != mEventCache.mCacheData.end(); ++it_topics)
            {
                auto event_code = it_topics->first;
                auto &topics = it_topics->second;
                for (auto it_data = topics.begin(); it_data != topics.end(); ++it_data)
                {
                    auto &topic = it_data->first;
                    auto &data = it_data->second;
                    auto cache_item = msg_cache.add_cache();
                    cache_item->set_event(event_code);
                    cache_item->set_topic(topic.c_str());
                    cache_item->set_size(data.mSize);
                }
            }
            CFdbParcelableBuilder builder(msg_cache);
            msg->replySideband(msg_ref, builder);
        }
        break;
        case FDB_SIDEBAND_KICK_WATCHDOG:
        {
            msg->forceRun(true);
            onKickDog(msg_ref);
        }
        break;
        case FDB_SIDEBAND_FEED_WATCHDOG:
            if (mWatchdog)
            {
                auto session = msg->getSession();
                if (session)
                {
                    mWatchdog->feedDog(session);
                }
            }
        break;
        default:
        break;
    }
}

class CPrepareDestroyJob : public CMethodJob<CFdbBaseObject>
{
public:
    CPrepareDestroyJob(CFdbBaseObject *object)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callPrepareDestroy, JOB_FORCE_RUN)
    {
    }
};

void CFdbBaseObject::callPrepareDestroy(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    enableMigrate(false);
}

void CFdbBaseObject::prepareDestroy()
{
    /*
     * Why not just call callPrepareDestroy()? Supposing the following case:
     * CFdbContext is executing the following logic:
     * 1. check if migration flag is set;
     * 2. if set, migrate callback to worker thread.
     * between 1 and 2, call callPrepareDestroy() followed by flush(). It might
     * be possible that the job to flush is in front of the job for migration.
     */
    mEndpoint->context()->sendSyncEndeavor(new CPrepareDestroyJob(this), 0, true);
    if (mWorker)
    {
        // Make sure no pending remote callback is queued
        mWorker->flush();
    }
}

class CDispOnOnlineJob : public CMethodJob<CFdbBaseObject>
{
public:
    CDispOnOnlineJob(CFdbBaseObject *object, const CFdbOnlineInfo &info, bool online,
                 CFdbBaseObject::tConnCallbackFn &callback)
        : CMethodJob<CFdbBaseObject>(object, &CFdbBaseObject::callDispOnOnline, JOB_FORCE_RUN)
        , mInfo(info)
        , mOnline(online)
        , mCallback(callback)
    {}

    CFdbOnlineInfo mInfo;
    bool mOnline;
    CFdbBaseObject::tConnCallbackFn mCallback;
};

void CFdbBaseObject::callDispOnOnline(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CDispOnOnlineJob *>(job);
    if (the_job)
    {
        the_job->mCallback(this, the_job->mInfo, the_job->mOnline);
    }
}

void CFdbBaseObject::migrateToWorker(const CFdbOnlineInfo &info, bool online,
                                     tConnCallbackFn &callback, CBaseWorker *worker)
{
    if (!callback)
    {
        return;
    }
    if (!worker || worker->isSelf())
    {
        try
        {
            callback(this, info, online);
        }
        catch (...)
        {
            LOG_E("CFdbBaseObject: except is caught at migrateToWorker for service %s!\n", mName.c_str());
        }
    }
    else
    {
        worker->sendAsync(new CDispOnOnlineJob(this, info, online, callback));
    }
}

CFdbBaseObject::tRegEntryId CFdbBaseObject::doRegisterConnNotification(tConnCallbackFn callback,
                                                                       CBaseWorker *worker)
{
    if (!callback)
    {
        return FDB_INVALID_ID;
    }
    CFdbBaseObject::tRegEntryId id = mRegIdAllocator++;
    auto &item = mConnCallbackTbl[id];
    item.mCallback = callback;
    item.mWorker = worker;

    if (!isPrimary())
    {
        return id;
    }

    bool is_first_secure = true;
    bool is_first_insecure = true;
    auto &containers = mEndpoint->getContainer();
    for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
    {
        auto container = socket_it->second;
        if (!container->mConnectedSessionTable.empty())
        {
            // get a snapshot of the table to avoid modification of the table in callback
            auto tbl = container->mConnectedSessionTable;
            for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
            {
                auto session = *session_it;
                if (!authentication(session))
                {
                    continue;
                }
                bool is_first;
                if (onlineSeparateChannel())
                {
                    if (session->isSecure())
                    {
                        is_first = is_first_secure;
                    }
                    else
                    {
                        is_first = is_first_insecure;
                    }
                }
                else
                {
                    is_first = is_first_secure && is_first_insecure;
                }
                if (session->isSecure())
                {
                    is_first_secure = false;
                }
                else
                {
                    is_first_insecure = false;
                }
                CFdbOnlineInfo info = {session->sid(),
                                       is_first,
                                       session->qos()};
                if ((mEndpoint->checkOnlineChannel(mOnlineChannelType, info)))
                {   // for client, only notify online/offline of secure channel
                    // if secure channel is enabled;
                    // for server, online/offline of all channels are notified;
                    // but the is_first/is_last takes all channel into account
                    LOG_I("CFdbBaseObject - server %s online: sid %d, QOS %d, is first: %d.\n",
                            mName.c_str(), info.mSid, info.mQOS, info.mFirstOrLast);
                    migrateToWorker(info, true, callback, worker);
                }
                is_first = false;
            }
        }
    }
    return id;
}

class CRegisterConnNotificationJob : public CMethodJob<CFdbBaseObject>
{
public:
    CRegisterConnNotificationJob(CFdbBaseObject *obj,
                     CFdbBaseObject::tConnCallbackFn &callback,
                     CBaseWorker *worker,
                     CFdbBaseObject::tRegEntryId &id)
        : CMethodJob<CFdbBaseObject>(obj, &CFdbBaseObject::callRegisterConnNotification, JOB_FORCE_RUN)
        , mCallback(callback)
        , mWorker(worker)
        , mId(id)
    {
    }
    CFdbBaseObject::tConnCallbackFn &mCallback;
    CBaseWorker *mWorker;
    CFdbBaseObject::tRegEntryId &mId;
};

void CFdbBaseObject::callRegisterConnNotification(CMethodJob<CFdbBaseObject> *job,
                                                  CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CRegisterConnNotificationJob *>(job);

    the_job->mId = doRegisterConnNotification(the_job->mCallback, the_job->mWorker);
}

CFdbBaseObject::tRegEntryId CFdbBaseObject::registerConnNotification(tConnCallbackFn callback, CBaseWorker *worker)
{
    tRegEntryId id = FDB_INVALID_ID;
    auto job = new CRegisterConnNotificationJob(this, callback, worker, id);
    mEndpoint->context()->sendSyncEndeavor(job);
    return id;
}

bool CFdbBaseObject::subscribeNoQueue(CFdbMsgSubscribeList &msg_list,
                                      CFdbMessage *msg,
                                      int32_t timeout)
{
    msg->type(FDB_MT_SUBSCRIBE_REQ);
    msg->setDestination(this);
    CFdbParcelableBuilder builder(msg_list);
    if (!msg->serialize(builder, this))
    {
        delete msg;
        return false;
    }
    msg->enableTimeStamp(timeStampEnabled());
    return msg->subscribeNoQueue(timeout);
}

bool CFdbBaseObject::subscribeEvents(const tEvtHandleTbl &events,
                                     tRegistryHandleTbl *reg_handle)
{
    CFdbMsgSubscribeList subscribe_list;
    for (auto it = events.begin(); it != events.end(); ++it)
    {
        subscribe_list.addNotifyItem(it->mCode, it->mTopic.c_str(), it->mFlag);
    }
    return subscribeNoQueue(subscribe_list, new CAFCSubscribeMsg(reg_handle));
}

bool CFdbBaseObject::doRegisterEventHandle(const CEvtHandleTbl &evt_tbl,
                                           tRegistryHandleTbl *reg_handle)
{
    tRegistryHandleTbl handle_tbl;
    mEventDispather.registerCallback(evt_tbl, &handle_tbl);
    if (reg_handle)
    {
        reg_handle->insert(reg_handle->end(), handle_tbl.begin(), handle_tbl.end());
    }

    if (mEndpoint->connected())
    {
        subscribeEvents(evt_tbl.getEvtHandleTbl(), &handle_tbl);
    }
    return true;
}

void CFdbBaseObject::replaceEventCache(FdbMsgCode_t id,
                                       const char *topic,
                                       const uint8_t *buffer,
                                       int32_t size,
                                       bool allow_event_route)
{
    mEventCache.replace(id, topic, buffer, size, allow_event_route);
}

class CRegisterEventHandleJob : public CMethodJob<CFdbBaseObject>
{
public:
    CRegisterEventHandleJob(CFdbBaseObject *obj,
                     const CEvtHandleTbl &evt_tbl,
                     tRegistryHandleTbl *reg_handle)
        : CMethodJob<CFdbBaseObject>(obj, &CFdbBaseObject::callRegisterEventHandle, JOB_FORCE_RUN)
        , mEvtTbl(evt_tbl)
        , mRegHandle(reg_handle)
    {
    }
    const CEvtHandleTbl &mEvtTbl;
    tRegistryHandleTbl *mRegHandle;
};

void CFdbBaseObject::callRegisterEventHandle(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CRegisterEventHandleJob *>(job);
    doRegisterEventHandle(the_job->mEvtTbl, the_job->mRegHandle);
}

bool CFdbBaseObject::registerEventHandle(const CEvtHandleTbl &evt_tbl,
                                         tRegistryHandleTbl *reg_handle)
{
    auto job = new CRegisterEventHandleJob(this, evt_tbl, reg_handle);
    return mEndpoint->context()->sendSyncEndeavor(job);
}

bool CFdbBaseObject::doRegisterMsgHandle(const CMsgHandleTbl &msg_tbl)
{
    return mMsgDispather.registerCallback(msg_tbl);
}

class CRegisterMsgHandleJob : public CMethodJob<CFdbBaseObject>
{
public:
    CRegisterMsgHandleJob(CFdbBaseObject *obj, const CMsgHandleTbl &msg_tbl)
        : CMethodJob<CFdbBaseObject>(obj, &CFdbBaseObject::callRegisterMsgHandle, JOB_FORCE_RUN)
        , mMsgTbl(msg_tbl)
    {
    }
    const CMsgHandleTbl &mMsgTbl;
};

void CFdbBaseObject::callRegisterMsgHandle(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CRegisterMsgHandleJob *>(job);
    doRegisterMsgHandle(the_job->mMsgTbl);
}

bool CFdbBaseObject::registerMsgHandle(const CMsgHandleTbl &msg_tbl)
{
    auto job = new CRegisterMsgHandleJob(this, msg_tbl);
    return mEndpoint->context()->sendSyncEndeavor(job);
}

bool CFdbBaseObject::doRegisterSetEvtHandle(const CEvtHandleTbl &setevt_tbl)
{
    return mSetEvtDispather.registerCallback(setevt_tbl);
}

class CRegisterSetEvtHandleJob : public CMethodJob<CFdbBaseObject>
{
public:
    CRegisterSetEvtHandleJob(CFdbBaseObject *obj, const CEvtHandleTbl &setevt_tbl)
        : CMethodJob<CFdbBaseObject>(obj, &CFdbBaseObject::callRegisterSetEvtHandle, JOB_FORCE_RUN)
        , mSetEvtTbl(setevt_tbl)
    {
    }
    const CEvtHandleTbl &mSetEvtTbl;
};

void CFdbBaseObject::callRegisterSetEvtHandle(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CRegisterSetEvtHandleJob *>(job);
    doRegisterSetEvtHandle(the_job->mSetEvtTbl);
}

bool CFdbBaseObject::registerSetEvtHandle(const CEvtHandleTbl &setevt_tbl)
{
    auto job = new CRegisterSetEvtHandleJob(this, setevt_tbl);
    return mEndpoint->context()->sendSyncEndeavor(job);
}

void CFdbBaseObject::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    const tRegistryHandleTbl *registered_evt_tbl = 0;
    auto *msg = castToMessage<CBaseMessage *>(msg_ref);
    if (msg->isInitialResponse() && (msg->getTypeId() == FDB_MSG_TYPE_AFC_SUBSCRIBE))
    {
        auto afc_msg = castToMessage<CAFCSubscribeMsg *>(msg_ref);
        registered_evt_tbl = &afc_msg->mRegHandle;
    }

    tEvtHandlePtrTbl handles_to_invoke;
    auto code = msg->code();
    mEventDispather.processMessage(msg_ref, code, this, handles_to_invoke, registered_evt_tbl);
    if (!fdbIsGroup(code))
    {
        mEventDispather.processMessage(msg_ref,
                                       fdbMakeGroup(code),
                                       this,
                                       handles_to_invoke,
                                       registered_evt_tbl);
    }
    if (!fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
    {
        mEventDispather.processMessage(msg_ref,
                                       fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS),
                                       this,
                                       handles_to_invoke,
                                       registered_evt_tbl);
    }
    mEventDispather.dispatchEvents(handles_to_invoke, msg_ref, msg, this);
}

void CFdbBaseObject::onOnline(const CFdbOnlineInfo &info)
{
    if (mRole == FDB_OBJECT_ROLE_CLIENT)
    {
        // TODO: select session according to QOS!
        publishCachedEvents(mEndpoint->preferredPeer(FDB_QOS_DEFAULT));
    }
    for (auto it = mConnCallbackTbl.begin(); it != mConnCallbackTbl.end(); ++it)
    {
        migrateToWorker(info, true, it->second.mCallback, it->second.mWorker);
    }
}
void CFdbBaseObject::onOffline(const CFdbOnlineInfo &info)
{
    for (auto it = mConnCallbackTbl.begin(); it != mConnCallbackTbl.end(); ++it)
    {
        migrateToWorker(info, false, it->second.mCallback, it->second.mWorker);
    }
}

void CFdbBaseObject::onInvoke(CBaseJob::Ptr &msg_ref)
{
    mMsgDispather.processMessage(msg_ref, this);
}

void CFdbBaseObject::onSetEvent(CBaseJob::Ptr &msg_ref)
{
    auto *msg = castToMessage<CBaseMessage *>(msg_ref);
    tEvtHandlePtrTbl handles_to_invoke;
    auto code = msg->code();
    mSetEvtDispather.processMessage(msg_ref, code, this, handles_to_invoke);
    if (!fdbIsGroup(code))
    {
        mSetEvtDispather.processMessage(msg_ref, fdbMakeGroup(code), this, handles_to_invoke);
    }
    if (!fdbEventInGroup(code, FDB_EVENT_ALL_GROUPS))
    {
        mSetEvtDispather.processMessage(msg_ref,
                                       fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS),
                                       this,
                                       handles_to_invoke);
    }
    mSetEvtDispather.dispatchEvents(handles_to_invoke, msg_ref, msg, this);
}

bool CFdbBaseObject::invoke(FdbMsgCode_t code
                           , IFdbMsgBuilder &data
                           , CFdbBaseObject::tInvokeCallbackFn callback
                           , CBaseWorker *worker
                           , int32_t timeout
                           , EFdbQOS qos)
{
    if (!callback)
    {
        return false;
    }
    auto invoke_msg = new CAFCInvokeMsg(code, callback, worker, qos);
    return CFdbBaseObject::invoke(invoke_msg, data, timeout);
}

bool CFdbBaseObject::invoke(FdbMsgCode_t code
                           , CFdbBaseObject::tInvokeCallbackFn callback
                           , const void *buffer
                           , int32_t size
                           , CBaseWorker *worker
                           , int32_t timeout
                           , EFdbQOS qos
                           , const char *log_info)
{
    if (!callback)
    {
        return false;
    }
    auto invoke_msg = new CAFCInvokeMsg(code, callback, worker, qos);
    invoke_msg->setLogData(log_info);
    return CFdbBaseObject::invoke(invoke_msg, buffer, size, timeout);
}

bool CFdbBaseObject::kickDog(CFdbSession *session)
{
    CFdbMessage msg(FDB_SIDEBAND_KICK_WATCHDOG, this);
    msg.expectReply(false);
    msg.enableLog(false);
    msg.type(FDB_MT_SIDEBAND_REQUEST);
    msg.serialize(0, 0, this);
    return session->sendMessage(&msg);
}

void CFdbBaseObject::onKickDog(CBaseJob::Ptr &msg_ref)
{
    CFdbMessage::feedDogNoQueue(msg_ref);
}

void CFdbBaseObject::onBark(CFdbSession *session)
{
    LOG_F("CFdbBaseObject: NAME %s, PID %d: NO RESPONSE!!!\n", session->getEndpointName().c_str(),
                                                               session->pid());
}

enum FdbWdogAction
{
    FDB_WDOG_START,
    FDB_WDOG_STOP,
    FDB_WDOG_REMOVE
};

class CWatchdogJob : public CMethodJob<CFdbBaseObject>
{
public:
    CWatchdogJob(CFdbBaseObject *obj, FdbWdogAction action, int32_t interval = 0, int32_t max_retries = 0)
        : CMethodJob<CFdbBaseObject>(obj, &CFdbBaseObject::callWatchdogAction, JOB_FORCE_RUN)
        , mAction(action)
        , mInterval(interval)
        , mMaxRetries(max_retries)
    {}
    FdbWdogAction mAction;
    int32_t mInterval;
    int32_t mMaxRetries;
};

void CFdbBaseObject::createWatchdog(int32_t interval, int32_t max_retries)
{
    if (!mWatchdog)
    {
        mWatchdog = new CFdbWatchdog(this, interval, max_retries);
        auto &containers = mEndpoint->getContainer();
        for (auto socket_it = containers.begin(); socket_it != containers.end(); ++socket_it)
        {
            auto container = socket_it->second;
            if (!container->mConnectedSessionTable.empty())
            {
                auto &tbl = container->mConnectedSessionTable;
                for (auto session_it = tbl.begin(); session_it != tbl.end(); ++session_it)
                {
                    auto session = *session_it;
                    mWatchdog->addDog(session);
                }
            }
        }
    }

    mWatchdog->start();
}

void CFdbBaseObject::callWatchdogAction(CMethodJob<CFdbBaseObject> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CWatchdogJob *>(job);
    if (the_job->mAction == FDB_WDOG_START)
    {
        createWatchdog(the_job->mInterval, the_job->mMaxRetries);
        mFlag |= FDB_OBJ_ENABLE_WATCHDOG;
    }
    else if (the_job->mAction == FDB_WDOG_STOP)
    {
        if (mWatchdog)
        {
            mWatchdog->stop();
        }
    }
    else if (the_job->mAction == FDB_WDOG_REMOVE)
    {
        if (mWatchdog)
        {
            delete mWatchdog;
            mWatchdog = 0;
            mFlag &= ~FDB_OBJ_ENABLE_WATCHDOG;
        }
    }
}

void CFdbBaseObject::startWatchdog(int32_t interval, int32_t max_retries)
{
    auto job = new CWatchdogJob(this, FDB_WDOG_START, interval, max_retries);
    mEndpoint->context()->sendAsync(job);
}

void CFdbBaseObject::stopWatchdog()
{
    auto job = new CWatchdogJob(this, FDB_WDOG_STOP);
    mEndpoint->context()->sendAsync(job);
}

void CFdbBaseObject::removeWatchdog()
{
    auto job = new CWatchdogJob(this, FDB_WDOG_REMOVE);
    mEndpoint->context()->sendAsync(job);
}

void CFdbBaseObject::getDroppedProcesses(CFdbMsgProcessList &process_list)
{
    if (mWatchdog)
    {
        mWatchdog->getDroppedProcesses(process_list);
    }
}
}
}
