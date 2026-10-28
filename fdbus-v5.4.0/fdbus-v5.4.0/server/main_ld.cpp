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

#include <fdbus/CFdbContext.h>
#include <fdbus/CDataPool.h>
#include <fdbus/CBaseClient.h>
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CBaseSysDep.h>
#include <fdbus/fdb_option_parser.h>
#include <iostream>
#include <string>

using namespace ipc::fdbus;

static void printTopics(FdbMsgTopics &topics)
{
    for (auto it = topics.topic_list().vpool().begin();
         it != topics.topic_list().vpool().end(); ++it)
    {
        printf("    pool name: %s, domain id: %d, topic name: %s; topic id: %d\n",
                it->topic_info().mPoolName.c_str(),
                it->topic_info().mDomainId,
                it->topic_info().mTopic.c_str(),
                it->topic_info().mTopicId);
    }
}

class CQueryClient : public CBaseClient
{
private:
    class CCtrlChannel : public CFdbBaseObject
    {
    protected:
        void onOnline(const CFdbOnlineInfo &info)
        {
            invoke(REQ_QUERY_TOPICS);
        }
        void onReply(CBaseJob::Ptr &msg_ref)
        {
            auto msg = castToMessage<CBaseMessage *>(msg_ref);
            if (msg->isStatus())
            {
                int32_t id;
                std::string reason;
                if (!msg->decodeStatus(id, reason))
                {
                    std::cout << "fail to decode status!" << std::endl;
                    exit(-1);
                }
                std::cout << "status is received: code: " << msg->code()
                          << ", reason: " << reason.c_str() << std::endl;
                exit(-1);
            }

            FdbMsgTopicsQuery query;
            CFdbParcelableParser parser(query);
            if (!msg->deserialize(parser))
            {
                std::cout << "unable to decode message!" << std::endl;
                exit(-1);
            }
            printf("\n=========== owned topics ===============\n");
            printTopics(query.mOwnedTopics);
            printf("=========== available topics ===============\n");
            printTopics(query.mAvailableTopics);
            exit(0);
        }
    };
    CCtrlChannel mCtrlChannel;
public:
    CQueryClient()
        : CBaseClient("query_client")
    {}
    bool connectToInstance(FdbInstanceId_t instance_id)
    {
        mCtrlChannel.connect(this, CTRL_OBJ);
        std::string url;
        CBaseSocketFactory::buildUrl(url, FDB_DATAPOOL_SERVER_NAME, instance_id);
        return connect(url.c_str());
    }
};

int main(int argc, char **argv)
{
    int32_t help = 0;
    int32_t domain_id = FDB_DEFAULT_DOMAIN;
    int32_t instance_id = -1;

    const struct fdb_option core_options[] = {
        { FDB_OPTION_INTEGER, "domain_id", 'd', &domain_id },
        { FDB_OPTION_INTEGER, "instance_id", 'i', &instance_id },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };

    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help)
    {
        std::cout << "FDBus - Fast Distributed Bus" << std::endl;
        std::cout << "    SDK version " << FDB_DEF_TO_STR(FDB_VERSION_MAJOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_MINOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_BUILD) << std::endl;
        std::cout << "    LIB version " << CFdbContext::getFdbLibVersion() << std::endl;
        std::cout << "Usage: lsdp[ -d domain_id][ -i instance_id]" << std::endl;
        exit(0);
    }

    FDB_CONTEXT->start();
    if (instance_id == -1)
    {
        CDataPool pool(domain_id, "query pool");
        pool.start();
        sysdep_sleep(2000);
        FdbMsgTopics owned_topics;
        FdbMsgTopics available_topics;
        pool.getTopicList(&owned_topics, &available_topics);
        printf("\n=========== owned topics ===============\n");
        printTopics(owned_topics);
        printf("=========== available topics ===============\n");
        printTopics(available_topics);
    }
    else
    {
        CQueryClient query_client;
        query_client.connectToInstance(instance_id);
        CBaseWorker background_worker;
        background_worker.start(FDB_WORKER_EXE_IN_PLACE);
    }
    return 0;
}

