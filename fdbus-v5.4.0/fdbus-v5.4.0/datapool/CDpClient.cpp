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
#include "CDpClient.h"

namespace ipc {
namespace fdbus {

CDpClient::CCtrlChannel::CCtrlChannel(CDpClient *dp_client)
    : CFdbBaseObject("DP_InfoFetcher")
    , mDpClient(dp_client)
{
}

void CDpClient::CCtrlChannel::connectWithDpClient()
{
    connect(mDpClient, CTRL_OBJ);
}

void CDpClient::CCtrlChannel::onOnline(const CFdbOnlineInfo &info)
{
    CFdbMsgSubscribeList subscribe_list;
    subscribe_list.addNotifyItem(NTF_TOPIC_CREATED);
    subscribe(subscribe_list, 0, info.mQOS);
}

void CDpClient::CCtrlChannel::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    switch (msg->code())
    {
        case NTF_TOPIC_CREATED:
        {
            FdbMsgTopics topic_list;
            CFdbParcelableParser parser(topic_list);
            if (!msg->deserialize(parser))
            {
                return;
            }
            mDpClient->updateTopicList(topic_list);
        }
        break;
        default:
        break;
    }
}

CDpClient::CDpClient(CDataPool *pool, const char *host_name, const char *ep_name,
                     CBaseWorker *handle_worker, CFdbBaseContext *context)
    : CBaseClient(ep_name, handle_worker, context)
    , mCtrlChannel(this)
    , mPool(pool)
    , mHostName(host_name)
{
    autoRemove(true);
    mCtrlChannel.connectWithDpClient();
}

CDpClient::~CDpClient()
{
    mPool->unregister(this);

    for (auto it_id = mTopicTbl.begin(); it_id != mTopicTbl.end(); ++it_id)
    {
        for (auto it_topic = it_id->second.begin(); it_topic != it_id->second.end(); ++it_topic)
        {
            auto &topic_info = it_topic->second;
            mPool->notifyTopicCreated(topic_info.mTopic.c_str(), topic_info.mTopicId, false);
        }
    }
}

bool CDpClient::topicCreated(const char *topic, FdbMsgCode_t topic_id)
{
    return mPool->topicCreated(topic, topic_id, mTopicTbl);
}

bool CDpClient::publishData(CFdbMessage *msg)
{
    if (!topicCreated(msg->topic().c_str(), msg->code()))
    {
        return false;
    }

    publishNoQueue(msg);
    return true;
}

bool CDpClient::subscribeData(FdbMsgCode_t topic_id, const char *topic,
                                std::vector<tDataPoolCallbackFn> &publish_listeners)
{
    if (!topicCreated(topic, topic_id))
    {
        return false;
    }
    for (auto it_func = publish_listeners.begin(); it_func != publish_listeners.end(); ++it_func)
    {
        CEvtHandleTbl evt_tbl;
        auto pool = mPool;
        auto publish_listener = *it_func;
        evt_tbl.add(topic_id, [pool, publish_listener](CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj){
                publish_listener(msg_ref, pool);
                },
                worker(),
                topic,
                pool->isNoReflectEnabled() ? FDB_SUBFLAG_NO_REFLECT : 0);
        doRegisterEventHandle(evt_tbl, 0);
        mPool->saveSubscribeHandleForClient(this, topic_id, topic, publish_listener);
    }
    return true;
}

void CDpClient::updateTopicList(FdbMsgTopics &topic_list)
{
    mTopicTbl.clear();
    topic_list.toTopicInfo(mTopicTbl);

    for (auto it = topic_list.topic_list().vpool().begin();
         it != topic_list.topic_list().vpool().end(); ++it)
    {
        mPool->onTopicCreated(it->topic_id(), it->topic().c_str(), this);
    }
}

void CDpClient::populateTopicList(FdbMsgTopics &topic_list)
{
    topic_list.fromTopicInfo(mTopicTbl);
}
}
}

