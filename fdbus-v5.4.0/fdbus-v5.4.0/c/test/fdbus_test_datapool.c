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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define FDB_LOG_TAG "FDB-SVC"
#include <fdbus/fdbus_clib.h>
#define FDB_CREATE_ALL_TEST 1
#define FDB_SUBSCRIBE_ALL_TEST 0

static const FdbDomainId_t fdb_domains[] = {0, 1, 2};
static const char *fdb_topic_set_a[] = {"topic1", "topic2", "topic3", "topic4"};
static const char *fdb_topic_set_b[] = {"topic11", "topic12", "topic13", "topic14"};
static FdbMsgCode_t fdb_topic_set_ids[] = {0, 1, 2, 3};
static int32_t fdb_test_mode = 0;

static void fdb_on_data_request(struct fdb_datapool_tag *handle, fdb_message_t *msg)
{
    FDB_LOG_I("fdb_on_data_request: domain: %d, sid: %d, code: %d, topic: %s, data: %s\n",
              fdb_dp_get_domain(handle), msg->sid, msg->msg_code, msg->topic, msg->msg_data);
    const char *data = "owner update and publish";
    fdb_dp_publish_data_ic(handle, msg->msg_code, msg->topic, (const uint8_t *)data, strlen(data) + 1,
                           fdb_true, FDB_QOS_RELIABLE);
}

static void fdb_on_data_publish(struct fdb_datapool_tag *handle, fdb_message_t *msg)
{
    FDB_LOG_I("fdb_on_data_publish: domain: %d, sid: %d, code: %d, topic: %s, data: %s\n",
              fdb_dp_get_domain(handle), msg->sid, msg->msg_code, msg->topic, msg->msg_data);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("help:\nfdbtestclibdatapool 0|1\n 0: mode 0; 1: mode 1\n");
        return -1;
    }

    fdb_start();
    fdb_test_mode = atoi(argv[1]);
    fdb_datapool_t *data_pools[Fdb_Num_Elems(fdb_domains)];
    for (int i = 0; i < Fdb_Num_Elems(fdb_domains); ++i)
    {
        data_pools[i] = fdb_dp_create(fdb_domains[i], "c-lib-datapool", 0);
        //fdb_dp_enable_noreflect(data_pools[i], fdb_true);
        fdb_dp_start(data_pools[i], fdb_true, fdb_true);
    }

    for (int i = 0; i < Fdb_Num_Elems(fdb_domains); ++i)
    {
#if FDB_CREATE_ALL_TEST
        //fdb_dp_create_data(data_pools[i], fdb_on_data_request);
        fdb_dp_create_data(data_pools[i], 0);
#else
        const char *init_data = "initial topic value";
        const char **own_topic_set = fdb_test_mode == 0 ? fdb_topic_set_a : fdb_topic_set_b;
        for (int j = 0; j < Fdb_Num_Elems(fdb_topic_set_ids); ++j)
        {
            fdb_dp_create_data_ic(data_pools[i], fdb_topic_set_ids[j], own_topic_set[j],
                                  fdb_on_data_request, (const uint8_t *)init_data, strlen(init_data) + 1);
        }
#endif
    }

    for (int i = 0; i < Fdb_Num_Elems(fdb_domains); ++i)
    {
#if FDB_SUBSCRIBE_ALL_TEST
        fdb_dp_subscribe_data(data_pools[i], fdb_on_data_publish);
#else
        const char **borrow_topic_set = fdb_test_mode == 0 ? fdb_topic_set_b : fdb_topic_set_a;
        for (int j = 0; j < Fdb_Num_Elems(fdb_topic_set_ids); ++j)
        {
            fdb_dp_subscribe_data_ic(data_pools[i], fdb_topic_set_ids[j], borrow_topic_set[j],
                                     fdb_on_data_publish);
        }
#endif
    }

    const char *publish_data = "published data";
    while (1)
    {
        for (int i = 0; i < Fdb_Num_Elems(fdb_domains); ++i)
        {
            const char **borrow_topic_set = fdb_test_mode == 0 ? fdb_topic_set_b : fdb_topic_set_a;
            for (int j = 0; j < Fdb_Num_Elems(fdb_topic_set_ids); ++j)
            {
                fdb_dp_publish_data_ic(data_pools[i], fdb_topic_set_ids[j], borrow_topic_set[j],
                                       (const uint8_t *)publish_data, strlen(publish_data) + 1, fdb_true, FDB_QOS_RELIABLE);
            }
        }
        sysdep_sleep(1000);
    }
}
