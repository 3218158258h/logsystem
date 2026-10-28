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
#include <string.h>
#include <fdbus/fdbus_datapool.h>
#include <fdbus/CDataPool.h>
#include <fdbus/CBaseWorker.h>
#define FDB_LOG_TAG "FDB_CLT"
#include <fdbus/fdb_log_trace.h>

using namespace ipc::fdbus;

fdb_datapool_t *fdb_dp_create(FdbDomainId_t domain_id, const char *pool_name, void *user_data)
{
    auto c_datapool = new fdb_datapool_t();
    memset(c_datapool, 0, sizeof(fdb_datapool_t));
    c_datapool->user_data = user_data;
    auto fdb_datapool = new CDataPool(domain_id, pool_name);
    c_datapool->native_handle = fdb_datapool;
    return c_datapool;
}

void fdb_dp_destroy(fdb_datapool_t *handle)
{
}

void fdb_dp_enable_noreflect(fdb_datapool_t *handle, fdb_bool_t active)
{
    if (!handle || !handle->native_handle)
    {
        return;
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;
    fdb_datapool->enableNoReflect(!!active);
}

fdb_bool_t fdb_dp_start(fdb_datapool_t *handle,
                        fdb_bool_t is_topic_owner,
                        fdb_bool_t is_topic_borrower)
{
    if (!handle || !handle->native_handle)
    {
        return false;
    }
    static CBaseWorker *worker = 0;
    if (!worker)
    {
        worker = new CBaseWorker();
        worker->start();
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;
    return fdb_datapool->start(worker, 0, is_topic_owner, is_topic_borrower);
}

FdbDomainId_t fdb_dp_get_domain(fdb_datapool_t *handle)
{
    if (!handle || !handle->native_handle)
    {
        return FDB_INVALID_ID;
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;
    return fdb_datapool->domainId();
}

fdb_bool_t fdb_dp_create_data_ic(fdb_datapool_t *handle,
                                 FdbMsgCode_t topic_id,
                                 const char *topic,
                                 fdb_data_request_fn_t on_data_request_fn,
                                 const uint8_t *init_data,
                                 int32_t size)
{
    if (!handle || !handle->native_handle)
    {
        return false;
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;
    bool createData(const char *topic,
                    const uint8_t *data = 0,
                    int32_t size = 0,
                    tDispatcherCallbackFn modify_listener = 0);
    if (!on_data_request_fn)
    {
        return fdb_datapool->createData(topic_id, topic, init_data, size, 0);
    }

    return fdb_datapool->createData(topic_id, topic, init_data, size,
                        [on_data_request_fn, handle](CBaseJob::Ptr &msg_ref, CDataPool *) {
                            auto msg = castToMessage<CFdbMessage *>(msg_ref);
                            fdb_message_t req_msg = {
                                     msg->session(),              //sid
                                     msg->code(),                 //msg_code
                                     msg->getPayloadBuffer(),     //msg_data
                                     msg->getPayloadSize(),       //data_size
                                     0,                           //status
                                     msg->topic().c_str(),        //topic
                                     0,                           //user_data
                                     msg->qos()                   //qos
                                     };
                            on_data_request_fn(handle, &req_msg);
                        });
}

fdb_bool_t fdb_dp_destroy_data_ic(fdb_datapool_t *handle,
                                  FdbMsgCode_t topic_id,
                                  const char *topic)
{
    if (!handle || !handle->native_handle)
    {
        return false;
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;
    return fdb_datapool->destroyData(topic_id, topic);
}

fdb_bool_t fdb_dp_publish_data_ic(fdb_datapool_t *handle,
                                  FdbMsgCode_t topic_id,
                                  const char *topic,
                                  const uint8_t *data,
                                  int32_t size,
                                  fdb_bool_t force_update,
                                  enum EFdbQOS qos)
{
    if (!handle || !handle->native_handle)
    {
        return false;
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;

    return fdb_datapool->publishData(topic_id, topic, data, size, force_update, qos);
}

fdb_bool_t fdb_dp_subscribe_data_ic(fdb_datapool_t *handle,
                                   FdbMsgCode_t topic_id,
                                   const char *topic,
                                   fdb_data_publish_notify_fn_t on_data_publish_fn)
{
    if (!handle || !handle->native_handle)
    {
        return false;
    }
    auto fdb_datapool = (CDataPool *)handle->native_handle;

    bool subscribeData(FdbMsgCode_t topic_id, const char *topic, tDispatcherCallbackFn publish_listener);
    return fdb_datapool->subscribeData(topic_id, topic,
                        [on_data_publish_fn, handle](CBaseJob::Ptr &msg_ref, CDataPool *){
                            auto msg = castToMessage<CFdbMessage *>(msg_ref);
                            fdb_message_t req_msg = {
                                     msg->session(),              //sid
                                     msg->code(),                 //msg_code
                                     msg->getPayloadBuffer(),     //msg_data
                                     msg->getPayloadSize(),       //data_size
                                     0,                           //status
                                     msg->topic().c_str(),        //topic
                                     0,                           //user_data
                                     msg->qos()                   //qos
                                     };
                            on_data_publish_fn(handle, &req_msg);
                        });
}

fdb_bool_t fdb_dp_create_data_i(fdb_datapool_t *handle,
                                FdbMsgCode_t topic_id,
                                fdb_data_request_fn_t on_data_request_fn,
                                const uint8_t *init_data,
                                int32_t size)
{
    return fdb_dp_create_data_ic(handle, topic_id, FDB_DP_DEFAULT_TOPIC_STR, on_data_request_fn, init_data, size);
}

fdb_bool_t fdb_dp_destroy_data_i(fdb_datapool_t *handle, FdbMsgCode_t topic_id)
{
    return fdb_dp_destroy_data_ic(handle, topic_id, FDB_DP_DEFAULT_TOPIC_STR);
}

fdb_bool_t fdb_dp_publish_data_i(fdb_datapool_t *handle,
                                 FdbMsgCode_t topic_id,
                                 const uint8_t *data,
                                 int32_t size,
                                 fdb_bool_t force_update,
                                 enum EFdbQOS qos)
{
    return fdb_dp_publish_data_ic(handle, topic_id, FDB_DP_DEFAULT_TOPIC_STR, data, size, force_update, qos);
}

fdb_bool_t fdb_dp_subscribe_data_i(fdb_datapool_t *handle,
                                   FdbMsgCode_t topic_id,
                                   fdb_data_publish_notify_fn_t on_data_publish_fn)
{
    return fdb_dp_subscribe_data_ic(handle, topic_id, FDB_DP_DEFAULT_TOPIC_STR, on_data_publish_fn);
}

fdb_bool_t fdb_dp_create_data_c(fdb_datapool_t *handle,
                                const char *topic,
                                fdb_data_request_fn_t on_data_request_fn,
                                const uint8_t *init_data,
                                int32_t size)
{
    return fdb_dp_create_data_ic(handle, FDB_DP_DEFAULT_TOPIC_ID, topic, on_data_request_fn, init_data, size);
}
fdb_bool_t fdb_dp_destroy_data_c(fdb_datapool_t *handle, const char *topic)
{
    return fdb_dp_destroy_data_ic(handle, FDB_DP_DEFAULT_TOPIC_ID, topic);
}
fdb_bool_t fdb_dp_publish_data_c(fdb_datapool_t *handle,
                                 const char *topic,
                                 const uint8_t *data,
                                 int32_t size,
                                 fdb_bool_t force_update,
                                 enum EFdbQOS qos)
{
    return fdb_dp_publish_data_ic(handle, FDB_DP_DEFAULT_TOPIC_ID, topic, data, size, force_update, qos);
}
fdb_bool_t fdb_dp_subscribe_data_c(fdb_datapool_t *handle,
                                   const char *topic,
                                   fdb_data_publish_notify_fn_t on_data_publish_fn)
{
    return fdb_dp_subscribe_data_ic(handle, FDB_DP_DEFAULT_TOPIC_ID, topic, on_data_publish_fn);
}

fdb_bool_t fdb_dp_create_data(fdb_datapool_t *handle,
                              fdb_data_request_fn_t on_data_request_fn)
{
    return fdb_dp_create_data_ic(handle, fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), 0, on_data_request_fn, 0, 0);
}

fdb_bool_t fdb_dp_subscribe_data(fdb_datapool_t *handle,
                                 fdb_data_publish_notify_fn_t on_data_publish_fn)
{
    return fdb_dp_subscribe_data_ic(handle, fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS), 0, on_data_publish_fn);
}
