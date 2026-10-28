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

#include <vector>
#define FDB_LOG_TAG "FDB_TEST_DATAPOOL"
#include <fdbus/fdbus.h>

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

int main(int argc, char **argv)
{
    char *offer_topics = 0;
    char *subscribe_topics = 0;
    char *domains = 0;
    char *name = 0;
    int32_t help = 0;
    const struct fdb_option core_options[] = {
        { FDB_OPTION_STRING, "subscribe_topics", 's', &subscribe_topics },
        { FDB_OPTION_STRING, "offer_topics", 'o', &offer_topics },
        { FDB_OPTION_STRING, "domains", 'd', &domains },
        { FDB_OPTION_STRING, "name", 'n', &name },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };
    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help)
    {
        printf("Usage: cmd -d domain1,domain2...[ -o offer_topic:count][ -s subscribe_topic:count]\n");
        return 0;
    }

    std::vector<FdbDomainId_t> domain_tbl;
    if (domains)
    {
        uint32_t num_domains;
        char **splitted = strsplit(domains, ",", &num_domains);
        for (uint32_t i = 0; i < num_domains; ++i)
        {
            domain_tbl.push_back(atoi(splitted[i]));
        }
    }
    else
    {
        domain_tbl.push_back(0);
    }

    int32_t offer_topic_id = 0;
    int32_t offer_topic_num = 0;
    if (offer_topics)
    {
        uint32_t num_items = 0;
        char **splitted = strsplit(offer_topics, ":", &num_items);
        if (num_items < 2)
        {
            printf("Bad format for offered topic: %s\n", offer_topics);
            exit(-1);
        }
        offer_topic_id = atoi(splitted[0]);
        offer_topic_num = atoi(splitted[1]);
    }

    int32_t subscribe_topic_id = 0;
    int32_t subscribe_topic_num = 0;
    if (subscribe_topics)
    {
        uint32_t num_items = 0;
        char **splitted = strsplit(subscribe_topics, ":", &num_items);
        if (num_items < 2)
        {
            printf("Bad format for subscribed topic: %s\n", subscribe_topics);
            exit(-1);
        }
        subscribe_topic_id = atoi(splitted[0]);
        subscribe_topic_num = atoi(splitted[1]);
    }

    FDB_CONTEXT->start();
    std::vector<CDataPool *> data_pool_table;
    for (auto it = domain_tbl.begin(); it < domain_tbl.end(); ++it)
    {
        auto data_pool = new CDataPool(*it, name);
        data_pool->start(CBaseWorker::create("my worker"),
                         CFdbBaseContext::create("my context"),
                         true,
                         true);
        data_pool_table.push_back(data_pool);
    }

    for (auto it = data_pool_table.begin(); it < data_pool_table.end(); ++it)
    {
        CDataPool *pool = *it;
        tDataPoolCallbackFn fn = [](CBaseJob::Ptr &msg_ref, CDataPool *pool)
                    {
                        auto msg = castToMessage<CFdbMessage *>(msg_ref);
                        printf("server receive data modification request - id: %d, topic: %s, data: %s\n",
                                msg->code(), msg->topic().c_str(), msg->getPayloadBuffer());
                        const char *content = "server modify data";
                        pool->publishData(msg->code(), content, (int32_t)strlen(content) + 1, true);
                    };

        if (offer_topic_num < 10)
        {
            for (int32_t i = 0; i < offer_topic_num; ++i)
            {
                const char *data = "server initialize topic pool";
                pool->createData(offer_topic_id + i, (const uint8_t *)data, (int32_t)strlen(data) + 1, fn);
            }
        }
        else
        {
            pool->createData(fn);
        }
    }

    //while (1)
    {
        for (auto it = data_pool_table.begin(); it < data_pool_table.end(); ++it)
        {
            CDataPool *pool = *it;
            FdbMsgTopics owned_topics;
            FdbMsgTopics available_topics;
            pool->getTopicList(&owned_topics, &available_topics);
        
            printf("\n=========== owned topics ===============\n");
            printTopics(owned_topics);
            printf("=========== available topics ===============\n");
            printTopics(available_topics);
        }
    }

    for (auto it = data_pool_table.begin(); it < data_pool_table.end(); ++it)
    {
        CDataPool *pool = *it;
        pool->registerTopicAvailableListener([](CTopicAvailableInfo &info)
            {
                if (info.mAvailable)
                {
                    printf("data pool: topic is created: id - %d, name: %s\n",
                           info.mTopicId, info.mTopic.c_str());
                }
                else
                {
                    printf("data pool: topic is destroyed: id - %d, name: %s\n",
                            info.mTopicId, info.mTopic.c_str());
                }
            });
        tDataPoolCallbackFn fn = [](CBaseJob::Ptr &msg_ref, CDataPool *pool) {
                            auto msg = castToMessage<CFdbMessage *>(msg_ref);
                            if (msg->getPayloadSize())
                            {
                                printf("client receive data publish - id: %d, topic: %s, data: %s\n",
                                        msg->code(), msg->topic().c_str(), msg->getPayloadBuffer());
                            }
                            else
                            {
                                printf("client receive data publish but no content!\n");
                            }
                        };
        if (subscribe_topic_num < 10)
        {
            for (int32_t i = 0; i < subscribe_topic_num; ++i)
            {
                pool->subscribeData(subscribe_topic_id + i,fn);
            }
        }
        else
        {
            // subscribe all data
            pool->subscribeData(fn);
        }
    }

    while (true)
    {
        for (auto it = data_pool_table.begin(); it < data_pool_table.end(); ++it)
        {
            CDataPool *pool = *it;
            for (int32_t i = 0; i < subscribe_topic_num; ++i)
            {
                const char *content = "client request to modify data";
                pool->publishData(subscribe_topic_id + i, content, (int32_t)strlen(content) + 1);
            }
        }
        sysdep_sleep(200);
    }
}

