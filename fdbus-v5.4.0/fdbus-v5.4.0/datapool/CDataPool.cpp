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
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CIntraNameProxy.h>
#include <utils/Log.h>
#include <string.h>

#include "CDpClient.h"
#include "CDpServer.h"

namespace ipc {
namespace fdbus {

class CServiceMonitor : public CBaseNotification<FdbMsgAddressList>
{
public:
    CServiceMonitor(CBaseWorker *worker, CDataPool *pool)
        : CBaseNotification<FdbMsgAddressList>(worker)
        , mPool(pool)
    {}
protected:
    void run(FdbMsgAddressList &msg_addr_list)
    {
        mPool->connect(msg_addr_list);
    }
private:
    CDataPool *mPool;
};

CDataPool::CDataPool(FdbDomainId_t domain_id, const char *name)
    : mName(name ? name : "DefaultPool")
    , mDomainId(domain_id)
    , mDpServer(0)
    , mHandleWorker(0)
    , mContext(0)
    , mSvcMonitor(0)
    , mNoReflect(false)
{
}

bool CDataPool::start(CBaseWorker *handle_worker,
                      CFdbBaseContext *context,
                      bool topic_owner,
                      bool topic_borrower)
{
    if (mContext)
    {
        return true;
    }
    else
    {
        mContext = context ? context : FDB_CONTEXT;
    }
    mHandleWorker = handle_worker;

    if (topic_owner)
    {
        mDpServer = new CDpServer(this, mName.c_str(), mHandleWorker, mContext);
        std::string url;
        CBaseSocketFactory::buildUrl(url, FDB_DATAPOOL_SERVER_NAME, fdbMakeGroupEvent(mDomainId));
        mDpServer->bind(url.c_str());
    }

    if (topic_borrower)
    {
        auto ns_proxy = FDB_CONTEXT->getNameProxy();
        if (ns_proxy)
        {
            ns_proxy->monitorService(new CServiceMonitor(mContext, this),
                                     FDB_DATAPOOL_SERVER_NAME,
                                     fdbMakeGroupEvent(mDomainId));
        }
    }

    return true;
}

void CDataPool::connect(FdbMsgAddressList &msg_addr_list)
{
    const char *host_name = msg_addr_list.host_name().c_str();
    const char *svc_name = msg_addr_list.service_name().c_str();
    FdbInstanceId_t instance_id = msg_addr_list.instance_id();
    bool is_local = msg_addr_list.is_local();
    bool disconnect = msg_addr_list.address_list().empty();

    if (fdbEventGroup(instance_id) != mDomainId)
    {
        return;
    }

    if (is_local && mDpServer && (mDpServer->instanceId() == instance_id))
     {  // do not connect to self. instance id is unique inside a host; however, for different
        // host, instance id might be the same. In this case, we shall still connect to it. If
        // 'is_local' is true, the service is located at the same host; thereby we shall not
        // connect with it because it is the service belong to the same data pool.
        return;
    }

    auto dp_client = findClient(host_name, svc_name, instance_id);
    if (dp_client)
    {
        dp_client->doDisconnect();
    }
    if (disconnect)
    {
        return;
    }

    if (!dp_client)
    {
        dp_client = new CDpClient(this, host_name, mName.c_str(), mHandleWorker, mContext);
        dp_client->setInstanceId(instance_id);
        dp_client->setNsName(FDB_DATAPOOL_SERVER_NAME);
    }

    // TODO: get UDP from name server!!!
    // This is just a workaround since we are not able to get UDP from NS monitor.
    auto &addr_list = msg_addr_list.address_list();
    for (auto it = addr_list.vpool().begin(); it != addr_list.vpool().end(); ++it)
    {
        if (it->has_udp_port())
        {
            it->set_udp_port(FDB_INET_PORT_AUTO);
        }
    }
    bool udp_failure = false;
    bool udp_success = false;
    bool tcp_failure = false;
    CIntraNameProxy::doConnectToAddress(dp_client, msg_addr_list, udp_failure, udp_success, tcp_failure);
    if (!tcp_failure)
    {
        mDpClientTbl.push_back(dp_client);
    }
}

CDpClient *CDataPool::findClient(const char *host_name, const char *svc_name, FdbInstanceId_t instance_id)
{
    for (auto it = mDpClientTbl.begin(); it != mDpClientTbl.end(); ++it)
    {
        auto client = *it;
        if ((client->instanceId() == instance_id) &&
            (client->nsName() == svc_name) &&
            (client->hostName() == host_name))
        {
            return client;
        }
    }
    return 0;
}

void CDataPool::unregister(CDpClient *client)
{
    for (auto it = mDpClientTbl.begin(); it != mDpClientTbl.end(); ++it)
    {
        if (*it == client)
        {
            mDpClientTbl.erase(it);
            break;
        }
    }
    auto it_subscribed = mClientSubscriptionHandle.find(client);
    if (it_subscribed == mClientSubscriptionHandle.end())
    {
        return;
    }

    for (auto it_id = it_subscribed->second.begin(); it_id != it_subscribed->second.end(); ++it_id)
    {
        for (auto it_topic = it_id->second.begin(); it_topic != it_id->second.end(); ++it_topic)
        {
            for (auto it_fn = it_topic->second.begin(); it_fn != it_topic->second.end(); ++it_fn)
            {
                doSubscribeData(it_id->first, it_topic->first.c_str(), *it_fn);
            }
        }
    }
    mClientSubscriptionHandle.erase(it_subscribed);
}

class createDataJob : public CMethodJob<CDataPool>
{
public:
    createDataJob(CDataPool *pool,
                  FdbMsgCode_t topic_id,
                  const char *topic,
                  uint8_t *buffer,
                  int32_t size,
                  tDataPoolCallbackFn modify_listener)
        : CMethodJob<CDataPool>(pool, &CDataPool::callCreateData, JOB_FORCE_RUN)
        , mTopicId(topic_id)
        , mTopic(topic)
        , mBuffer(buffer)
        , mSize(size)
        , mModifyListener(modify_listener)
    {
    }
    FdbMsgCode_t mTopicId;
    std::string mTopic;
    uint8_t *mBuffer;
    int32_t mSize;
    tDataPoolCallbackFn mModifyListener;
};

void CDataPool::callCreateData(CMethodJob<CDataPool> *job, CBaseJob::Ptr &ref)
{
    if (!mDpServer)
    {
        return;
    }

    auto the_job = fdb_dynamic_cast_if_available<createDataJob *>(job);
    mDpServer->createData(the_job->mTopicId,
                          the_job->mTopic.c_str(),
                          the_job->mBuffer,
                          the_job->mSize,
                          the_job->mModifyListener);
}

bool CDataPool::createData(FdbMsgCode_t topic_id,
                           const char *topic,
                           IFdbMsgBuilder &data,
                           tDataPoolCallbackFn modify_listener)
{
    if (!mContext)
    {
        LOG_E("createData: data pool is not started!\n");
        return false;
    }
    int32_t size = data.build();
    if (size < 0)
    {
        return false;
    }
    uint8_t *raw_data = 0;
    try
    {
        raw_data = new uint8_t[size];
    }
    catch (...)
    {
        LOG_E("CDataPool: fail to allocate event cache with size %d\n", size);
    }
    if (raw_data)
    {
        data.toBuffer(raw_data, size);
    }
    else
    {
        return false;
    }

    if (!topic)
    {
        topic = "";
    }

    return mContext->sendAsyncEndeavor(new createDataJob(this, topic_id, topic, raw_data, size, modify_listener));
}

bool CDataPool::createData(FdbMsgCode_t topic_id,
                           const char *topic,
                           const uint8_t *data,
                           int32_t size,
                           tDataPoolCallbackFn modify_listener)
{
    if (!mContext)
    {
        LOG_E("createData: data pool is not started!\n");
        return false;
    }
    uint8_t *raw_data = 0;
    if (data && size)
    {
        try
        {
            raw_data = new uint8_t[size];
        }
        catch (...)
        {
            LOG_E("CDataPool: fail to allocate event cache with size %d\n", size);
        }
    }
    if (raw_data)
    {
        memcpy(raw_data, data, size);
    }

    if (!topic)
    {
        topic = "";
    }
    return mContext->sendAsyncEndeavor(new createDataJob(this, topic_id, topic, raw_data, size, modify_listener));
}

class destroyDataJob : public CMethodJob<CDataPool>
{
public:
    destroyDataJob(CDataPool *pool,
                  FdbMsgCode_t topic_id,
                  const char *topic)
        : CMethodJob<CDataPool>(pool, &CDataPool::callDestroyData, JOB_FORCE_RUN)
        , mTopicId(topic_id)
        , mTopic(topic)
    {
    }
    FdbMsgCode_t mTopicId;
    std::string mTopic;
};

void CDataPool::callDestroyData(CMethodJob<CDataPool> *job, CBaseJob::Ptr &ref)
{
    if (!mDpServer)
    {
        return;
    }
    auto the_job = fdb_dynamic_cast_if_available<destroyDataJob *>(job);
    mDpServer->destroyData(the_job->mTopicId, the_job->mTopic.c_str());
}

bool CDataPool::destroyData(FdbMsgCode_t topic_id, const char *topic)
{
    if (!mContext)
    {
        LOG_E("destroyData: data pool is not started!\n");
        return false;
    }
    if (!topic)
    {
        topic = "";
    }

    return mContext->sendAsyncEndeavor(new destroyDataJob(this, topic_id, topic));
}

bool CDataPool::doPublishData(CFdbMessage *msg)
{
    msg->setCallable([this](CBaseJob::Ptr &msg_ref)
        {
            auto msg = castToMessage<CBaseMessage *>(msg_ref);
            if (!this->mDpServer || !this->mDpServer->publishData(msg))
            {
                for (auto it = this->mDpClientTbl.begin(); it != this->mDpClientTbl.end(); ++it)
                {
                    auto client = *it;
                    if (client->publishData(msg))
                    {
                        break;
                    }
                }
            }
        });
    return mContext->sendAsyncEndeavor(msg);
}

bool CDataPool::publishData(FdbMsgCode_t topic_id, const char *topic, IFdbMsgBuilder &data,
                            bool force_update, EFdbQOS qos)
{
    if (!mContext)
    {
        LOG_E("publishData: data pool is not started!\n");
        return false;
    }
    auto msg = new CBaseMessage(topic_id, qos);
    msg->topic(topic);
    if (!msg->serialize(data))
    {
        delete msg;
        return false;
    }
    msg->forceUpdate(force_update);
    return doPublishData(msg);
}

bool CDataPool::publishData(FdbMsgCode_t topic_id, const char *topic, const void *data, int32_t size,
                 bool force_update, EFdbQOS qos)
{
    if (!mContext)
    {
        LOG_E("publishData: data pool is not started!\n");
        return false;
    }
    auto msg = new CBaseMessage(topic_id, qos);
    msg->topic(topic);
    if (!msg->serialize(data, size))
    {
        delete msg;
        return false;
    }
    msg->forceUpdate(force_update);
    return doPublishData(msg);
}

class subscribeDataJob : public CMethodJob<CDataPool>
{
public:
    subscribeDataJob(CDataPool *pool,
                  FdbMsgCode_t topic_id,
                  const char *topic,
                  tDataPoolCallbackFn publish_listener)
        : CMethodJob<CDataPool>(pool, &CDataPool::callSubscribeData, JOB_FORCE_RUN)
        , mTopicId(topic_id)
        , mTopic(topic)
        , mPublishListener(publish_listener)
    {
    }
    FdbMsgCode_t mTopicId;
    std::string mTopic;
    tDataPoolCallbackFn mPublishListener;
};

void CDataPool::doSubscribeData(FdbMsgCode_t topic_id, const char *topic, tDataPoolCallbackFn publish_listener)
{
    bool subscribed = false;
    std::vector<tDataPoolCallbackFn> listeners;
    listeners.push_back(publish_listener);
    for (auto it = mDpClientTbl.begin(); it != mDpClientTbl.end(); ++it)
    {
        auto client = *it;
        if (client->subscribeData(topic_id, topic, listeners))
        {
            subscribed = true;
            break;
        }
    }

    if (!subscribed)
    {
        mPendingSubscriptionHandle[topic_id][topic].push_back(publish_listener);
    }
}

void CDataPool::callSubscribeData(CMethodJob<CDataPool> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<subscribeDataJob *>(job);
    doSubscribeData(the_job->mTopicId, the_job->mTopic.c_str(), the_job->mPublishListener);
}

bool CDataPool::topicMatched(FdbMsgCode_t id_intended, FdbMsgCode_t it_stored)
{
    if (it_stored == id_intended)
    {
        return true;
    }
    if (fdbIsGroup(id_intended) || fdbIsGroup(it_stored))
    {
        if (fdbEventInGroup(id_intended, FDB_EVENT_ALL_GROUPS) ||
                fdbEventInGroup(it_stored, FDB_EVENT_ALL_GROUPS))
        {
            return true;
        }
        if (fdbSameGroup(id_intended, it_stored))
        {
            return true;
        }
    }

    return false;
}

bool CDataPool::topicMatched(const char *topic_intended, const std::string &topic_stored)
{
    if (topic_stored == topic_intended)
    {
        return true;
    }
    if (topic_stored.empty() || (topic_intended[0] == '\0'))
    {
        return true;
    }

    return false;
}

void CDataPool::onTopicCreated(FdbMsgCode_t topic_id, const char *topic, CDpClient *client)
{
    for (auto it_id = mPendingSubscriptionHandle.begin(); it_id != mPendingSubscriptionHandle.end();)
    {
        auto the_it_id = it_id;
        ++it_id;

        auto subs_id = the_it_id->first;
        if (!topicMatched(topic_id, subs_id))
        {
            continue;
        }

        auto &id_list = the_it_id->second;
        for (auto it_topic = id_list.begin(); it_topic != id_list.end();)
        {
            auto the_it_topic = it_topic;
            ++it_topic;

            auto &subs_topic = the_it_topic->first;
            if (!topicMatched(topic, subs_topic))
            {
                continue;
            }

            if (client->subscribeData(subs_id, subs_topic.c_str(), the_it_topic->second))
            {
                id_list.erase(the_it_topic);
            }
        }
        if (id_list.empty())
        {
            mPendingSubscriptionHandle.erase(the_it_id);
        }
    }

    notifyTopicCreated(topic, topic_id, true);
}

bool CDataPool::subscribeData(FdbMsgCode_t topic_id, const char *topic, tDataPoolCallbackFn publish_listener)
{
    if (!mContext)
    {
        LOG_E("subscribeData: data pool is not started!\n");
        return false;
    }
    if (!topic)
    {
        topic = "";
    }

    return mContext->sendAsyncEndeavor(new subscribeDataJob(this, topic_id, topic, publish_listener));
}

void CDataPool::doGetTopicList(FdbMsgTopics *owned_topics, FdbMsgTopics *available_topics)
{
    if (mDpServer && owned_topics)
    {
        mDpServer->populateTopicList(*owned_topics);
    }

    if (available_topics)
    {
        for (auto it = this->mDpClientTbl.begin(); it != this->mDpClientTbl.end(); ++it)
        {
            auto client = *it;
            client->populateTopicList(*available_topics);
        }
    }
}

class getTopicListJob : public CMethodJob<CDataPool>
{
public:
    getTopicListJob(CDataPool *pool,
                    FdbMsgTopics *owned_topics,
                    FdbMsgTopics *available_topics)
        : CMethodJob<CDataPool>(pool, &CDataPool::callGetTopicList, JOB_FORCE_RUN)
        , mOwnedTopics(owned_topics)
        , mAvailableTopics(available_topics)
    {
    }
    FdbMsgTopics *mOwnedTopics;
    FdbMsgTopics *mAvailableTopics;
};

void CDataPool::callGetTopicList(CMethodJob<CDataPool> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<getTopicListJob *>(job);
    doGetTopicList(the_job->mOwnedTopics, the_job->mAvailableTopics);
}

void CDataPool::getTopicList(FdbMsgTopics *owned_topics, FdbMsgTopics *available_topics)
{
    mContext->sendSyncEndeavor(new getTopicListJob(this, owned_topics, available_topics));
}

bool CDataPool::createData(const char *topic,
                           IFdbMsgBuilder &data,
                           tDataPoolCallbackFn modify_listener)
{
    return createData(FDB_DP_DEFAULT_TOPIC_ID, topic, data, modify_listener);
}

bool CDataPool::createData(const char *topic,
                           const uint8_t *data,
                           int32_t size,
                           tDataPoolCallbackFn modify_listener)
{
    return createData(FDB_DP_DEFAULT_TOPIC_ID, topic, data, size, modify_listener);
}

bool CDataPool::destroyData(const char *topic)
{
    return destroyData(FDB_DP_DEFAULT_TOPIC_ID, topic);
}

bool CDataPool::publishData(const char *topic, IFdbMsgBuilder &data,
                            bool force_update, EFdbQOS qos)
{
    return publishData(FDB_DP_DEFAULT_TOPIC_ID, topic, data, force_update, qos);
}

bool CDataPool::publishData(const char *topic, const void *data, int32_t size,
                            bool force_update, EFdbQOS qos)
{
    return publishData(FDB_DP_DEFAULT_TOPIC_ID, topic, data, size, force_update, qos);
}

bool CDataPool::subscribeData(const char *topic, tDataPoolCallbackFn publish_listener)
{
    return subscribeData(FDB_DP_DEFAULT_TOPIC_ID, topic, publish_listener);
}

bool CDataPool::createData(FdbMsgCode_t topic_id,
                           IFdbMsgBuilder &data,
                           tDataPoolCallbackFn modify_listener)
{
    return createData(topic_id, FDB_DP_DEFAULT_TOPIC_STR, data, modify_listener);
}

bool CDataPool::createData(FdbMsgCode_t topic_id,
                           const uint8_t *data,
                           int32_t size,
                           tDataPoolCallbackFn modify_listener)
{
    return createData(topic_id, FDB_DP_DEFAULT_TOPIC_STR, data, size, modify_listener);
}

bool CDataPool::destroyData(FdbMsgCode_t topic_id)
{
    return destroyData(topic_id, FDB_DP_DEFAULT_TOPIC_STR);
}

bool CDataPool::publishData(FdbMsgCode_t topic_id, IFdbMsgBuilder &data,
                            bool force_update, EFdbQOS qos)
{
    return publishData(topic_id, FDB_DP_DEFAULT_TOPIC_STR, data, force_update, qos);
}

bool CDataPool::publishData(FdbMsgCode_t topic_id, const void *data, int32_t size,
                            bool force_update, EFdbQOS qos)
{
    return publishData(topic_id, FDB_DP_DEFAULT_TOPIC_STR, data, size, force_update, qos);
}

bool CDataPool::subscribeData(FdbMsgCode_t topic_id, tDataPoolCallbackFn publish_listener)
{
    return subscribeData(topic_id, FDB_DP_DEFAULT_TOPIC_STR, publish_listener);
}

class CTopicAvailableListener : public CBaseNotification<CTopicAvailableInfo>
{
public:
    CTopicAvailableListener(CBaseWorker *worker, tTopicAvailableFn listener_fn)
        : CBaseNotification<CTopicAvailableInfo>(worker)
        , mListenerFn(listener_fn)
    {}
protected:
    void run(CTopicAvailableInfo &data)
    {
        if (mListenerFn)
        {
            mListenerFn(data);
        }
    }
private:
    tTopicAvailableFn mListenerFn;
};

void CDataPool::notifyTopicCreated(const char *topic, FdbMsgCode_t topic_id, bool created)
{
    CTopicAvailableInfo available_data = {topic, topic_id, this, created};
    mTopicAvailableNtfCenter.notify(available_data);
}

tTopicAvailableHandler CDataPool::registerTopicAvailableListener(tTopicAvailableFn listener)
{
    auto notification = new CTopicAvailableListener(mHandleWorker, listener);
    mContext->sendAsyncEndeavor([this, notification](CBaseJob::Ptr &job)
        {
            CBaseNotification<CTopicAvailableInfo>::Ptr ntf(notification);
            this->mTopicAvailableNtfCenter.subscribe(ntf);
            for (auto it = this->mDpClientTbl.begin(); it != this->mDpClientTbl.end(); ++it)
            {
                auto client = *it;
                auto &topic_tbl = client->getCreatedTopicList();
                for (auto it_id = topic_tbl.begin(); it_id != topic_tbl.end(); ++it_id)
                {
                    for (auto it_info = it_id->second.begin(); it_info != it_id->second.end(); ++it_info)
                    {
                        auto &topic_info = it_info->second;
                        CTopicAvailableInfo available_data =
                                    {topic_info.mTopic.c_str(), topic_info.mTopicId, this, true};
                        mTopicAvailableNtfCenter.notify(available_data, ntf);
                    }
                }
            }
        }, false, JOB_FORCE_RUN);

    return (tTopicAvailableHandler)notification;
}

bool CDataPool::topicCreated(const char *topic, FdbMsgCode_t topic_id, tTopicTbl &topic_tbl)
{
    auto it_id = topic_tbl.find(topic_id);
    if (it_id != topic_tbl.end())
    {
        auto it_topic = it_id->second.find(topic);
        if (it_topic != it_id->second.end())
        {
            return true;
        }
    }
    for (auto it_id = topic_tbl.begin(); it_id != topic_tbl.end(); ++it_id)
    {
        for (auto it_topic = it_id->second.begin();
                it_topic != it_id->second.end(); ++it_topic)
        {
            std::string &topic_created = it_topic->second.mTopic;
            FdbMsgCode_t topic_id_created = it_topic->second.mTopicId;
            if (topic_created.empty() || (topic_created == topic))
            {
                if (topic_id_created == topic_id)
                {
                    return true;
                }
                if (fdbIsGroup(topic_id_created))
                {
                    if (fdbEventInGroup(topic_id_created, FDB_EVENT_ALL_GROUPS))
                    {
                        return true;
                    }
                    if (fdbSameGroup(topic_id_created, topic_id))
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool CDataPool::subscribeData(tDataPoolCallbackFn publish_listener)
{
    return subscribeData(fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), 0, publish_listener);
}

bool CDataPool::createData(tDataPoolCallbackFn modify_listener)
{
    return createData(fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), 0, 0, 0, modify_listener);
}

bool CDataPool::destroyData()
{
    return destroyData();
}

void CDataPool::saveSubscribeHandleForClient(CDpClient *client,
                                             FdbMsgCode_t topic_id,
                                             const char *topic,
                                             tDataPoolCallbackFn publish_listener)
{
    mClientSubscriptionHandle[client][topic_id][topic].push_back(publish_listener); 
}

}
}

