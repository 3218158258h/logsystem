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

#include <fdbus/CDataPool.h>
//#include <fdbus/CFdbMessage.h>
#include "CDpServer.h"

namespace ipc {
namespace fdbus {

CDpServer::CCtrlChannel::CCtrlChannel(CDpServer *dp_server)
    : CFdbBaseObject("DP_Publisher")
    , mDpServer(dp_server)
{
}

void CDpServer::CCtrlChannel::bindToDpServer()
{
    bind(mDpServer, CTRL_OBJ);
}

void CDpServer::CCtrlChannel::onInvoke(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CBaseMessage *>(msg_ref);

    switch (msg->code())
    {
        case REQ_QUERY_TOPICS:
        {
            FdbMsgTopicsQuery query;
            mDpServer->mPool->getTopicList(&query.mOwnedTopics, &query.mAvailableTopics);
            CFdbParcelableBuilder builder(query);
            msg->reply(msg_ref, builder);
        }
        break;
        default:
        break;
    }
}

void CDpServer::CCtrlChannel::onSubscribe(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    const CFdbMsgSubscribeItem *sub_item;
    /* iterate all message id subscribed */
    FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
    {
        switch (sub_item->msg_code())
        {
            case NTF_TOPIC_CREATED:
            {
                FdbMsgTopics topic_list;
                mDpServer->populateTopicList(topic_list);
                CFdbParcelableBuilder builder(topic_list);
                msg->broadcast(NTF_TOPIC_CREATED, builder);
            }
            break;
            default:
            break;
        }
    }
    FDB_END_FOREACH_SIGNAL()
}

CDpServer::CDpServer(CDataPool *pool, const char *ep_name, CBaseWorker *handle_worker,
                     CFdbBaseContext *context)
    : CBaseServer(ep_name, handle_worker, context)
    , mCtrlChannel(this)
    , mPool(pool)
{
    enableEventCache(true);
    mCtrlChannel.bindToDpServer();
}

void CDpServer::populateTopicList(FdbMsgTopics &topic_list)
{
    topic_list.fromTopicInfo(mTopicTbl);
}

bool CDpServer::publishData(CFdbMessage *msg)
{
    if (!mPool->topicCreated(msg->topic().c_str(), msg->code(), mTopicTbl))
    {
        return false;
    }

    return publishNoQueue(msg);
}

bool CDpServer::createData(FdbMsgCode_t topic_id,
                           const char *topic,
                           const uint8_t *data,
                           int32_t size,
                           tDataPoolCallbackFn modify_listener)
{
    if (mPool->topicCreated(topic, topic_id, mTopicTbl))
    {
        return true;
    }

    auto &topic_info = mTopicTbl[topic_id][topic];
    topic_info.mTopic = topic;
    topic_info.mTopicId = topic_id;
    topic_info.mPoolName = name();
    topic_info.mDomainId = mPool->domainId();

    if (modify_listener)
    {
        CEvtHandleTbl evt_tbl;
        auto pool = mPool;
        evt_tbl.add(topic_id, [pool, modify_listener](CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj){
             modify_listener(msg_ref, pool);
             }, worker(), topic);
        doRegisterSetEvtHandle(evt_tbl);
    }

    bool allow_event_route = !modify_listener;
    if ((topic_id == fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS)) && (topic[0] == '\0'))
    {
        enableEventRoute(allow_event_route);
    }
    else
    {
        replaceEventCache(topic_id, topic, data, size, allow_event_route);
    }

    // broadcast updated topic table
    FdbMsgTopics topic_list;
    populateTopicList(topic_list);
    CFdbParcelableBuilder builder(topic_list);
    mCtrlChannel.broadcast(NTF_TOPIC_CREATED, builder);
    return true;
}

void CDpServer::destroyData(FdbMsgCode_t topic_id, const char *topic)
{
    bool destroyed = false;
    auto it_id = mTopicTbl.find(topic_id);
    if (it_id != mTopicTbl.end())
    {
        auto it_topic = it_id->second.find(topic);
        if (it_topic != it_id->second.end())
        {
            it_id->second.erase(it_topic);
        }
        if (it_id->second.empty())
        {
            mTopicTbl.erase(it_id);
            destroyed = true;
        }
    }

    if (destroyed)
    {
        // broadcast updated topic table
        FdbMsgTopics topic_list;
        populateTopicList(topic_list);
        CFdbParcelableBuilder builder(topic_list);
        mCtrlChannel.broadcast(NTF_TOPIC_CREATED, builder);
    }
}
}
}

