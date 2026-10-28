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
#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CLogProducer.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/fdbus_client.h>
#define FDB_LOG_TAG "FDB_CLT"
#include <fdbus/fdb_log_trace.h>

#define FDB_MSG_TYPE_C_INVOKE (FDB_MSG_TYPE_SYSTEM + 1)
namespace ipc {
namespace fdbus {

class CCClient : public CBaseClient
{
public:
    CCClient(const char *name, fdb_client_t *c_handle = 0);
    ~CCClient();
protected:
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);
    void onReply(CBaseJob::Ptr &msg_ref);
    void onGetEvent(CBaseJob::Ptr &msg_ref);
    void onBroadcast(CBaseJob::Ptr &msg_ref);
private:
    fdb_client_t *mClient;
};

class CCInvokeMsg : public CBaseMessage
{
public:
    CCInvokeMsg(FdbMsgCode_t code, void *user_data, enum EFdbQOS qos)
        : CBaseMessage(code, qos)
        , mUserData(user_data)
    {
    }
    FdbMessageType_t getTypeId()
    {
        return FDB_MSG_TYPE_C_INVOKE;
    }
    void *mUserData;
};

CBaseClient *FDB_createCClient(const char *name)
{
    return new CCClient(name);
}

CCClient::CCClient(const char *name, fdb_client_t *c_handle)
    : CBaseClient(name)
    , mClient(c_handle)
{
    enableReconnect(true);
}

CCClient::~CCClient()
{
}

void CCClient::onOnline(const CFdbOnlineInfo &info)
{
    if (!mClient)
    {
        CFdbBaseObject::onOnline(info);
        return;
    }

    if (!mClient->handles || !mClient->handles->on_online_func)
    {
        return;
    }

    mClient->handles->on_online_func(mClient, info.mSid, info.mQOS);
}

void CCClient::onOffline(const CFdbOnlineInfo &info)
{
    if (!mClient)
    {
        CFdbBaseObject::onOffline(info);
        return;
    }

    if (!mClient->handles || !mClient->handles->on_offline_func)
    {
        return;
    }
    mClient->handles->on_offline_func(mClient, info.mSid, info.mQOS);
}

void CCClient::onReply(CBaseJob::Ptr &msg_ref)
{
    auto *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    if (!mClient || (fdb_msg->getTypeId() < 0))
    {
        CFdbBaseObject::onReply(msg_ref);
        return;
    }

    if (!mClient->handles || !mClient->handles->on_reply_func)
    {
        return;
    }

    int32_t error_code = FDB_ST_OK;
    if (fdb_msg->isStatus())
    {
        std::string reason;
        if (!fdb_msg->decodeStatus(error_code, reason))
        {
            FDB_LOG_E("onReply: fail to decode status!\n");
            error_code = FDB_ST_MSG_DECODE_FAIL;
        }
    }

    CCInvokeMsg *c_msg = 0;
    if (fdb_msg->getTypeId() == FDB_MSG_TYPE_C_INVOKE)
    {
        c_msg = castToMessage<CCInvokeMsg *>(msg_ref);
    }

    fdb_message_t reply_msg = {fdb_msg->session(),              //sid
                               fdb_msg->code(),                 //msg_code
                               fdb_msg->getPayloadBuffer(),     //msg_data
                               fdb_msg->getPayloadSize(),       //data_size
                               error_code,                      //status
                               0,                               //topic
                               c_msg ? c_msg->mUserData : 0,    //user_data
                               fdb_msg->qos(),                  //qos
                               0                                //_msg_buffer
                               };

    mClient->handles->on_reply_func(mClient, &reply_msg);
}

void CCClient::onGetEvent(CBaseJob::Ptr &msg_ref)
{
    if (!mClient || !mClient->handles || !mClient->handles->on_get_event_func)
    {
        return;
    }

    auto *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    int32_t error_code = FDB_ST_OK;
    if (fdb_msg->isStatus())
    {
        std::string reason;
        if (!fdb_msg->decodeStatus(error_code, reason))
        {
            FDB_LOG_E("onReply: fail to decode status!\n");
            error_code = FDB_ST_MSG_DECODE_FAIL;
        }
    }

    CCInvokeMsg *c_msg = 0;
    if (fdb_msg->getTypeId() == FDB_MSG_TYPE_C_INVOKE)
    {
        c_msg = castToMessage<CCInvokeMsg *>(msg_ref);
    }

    fdb_message_t reply_msg = {fdb_msg->session(),              //sid
                               fdb_msg->code(),                 //msg_code
                               fdb_msg->getPayloadBuffer(),     //msg_data
                               fdb_msg->getPayloadSize(),       //data_size
                               error_code,                      //status
                               fdb_msg->topic().c_str(),        //topic
                               c_msg ? c_msg->mUserData : 0,    //user_data
                               fdb_msg->qos(),                  //qos
                               0                                //_msg_buffer
                               };

    mClient->handles->on_get_event_func(mClient, &reply_msg);
}

void CCClient::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    if (!mClient)
    {
        CFdbBaseObject::onBroadcast(msg_ref);
        return;
    }

    if (!mClient->handles || !mClient->handles->on_broadcast_func)
    {
        return;
    }
    auto *fdb_msg = castToMessage<CBaseMessage *>(msg_ref);
    if (fdb_msg)
    {
        fdb_message_t reply_msg = {fdb_msg->session(),              //sid
                                   fdb_msg->code(),                 //msg_code
                                   fdb_msg->getPayloadBuffer(),     //msg_data
                                   fdb_msg->getPayloadSize(),       //data_size
                                   0,                               //status
                                   fdb_msg->topic().c_str(),        //topic
                                   0,                               //user_data
                                   fdb_msg->qos(),                  //qos
                                   0                                //_msg_buffer
                                   };
        mClient->handles->on_broadcast_func(mClient, &reply_msg);
    }
}
}
}
using namespace ipc::fdbus;

fdb_client_t *fdb_client_create(const char *name, void *user_data)
{
    auto c_client = new fdb_client_t();
    memset(c_client, 0, sizeof(fdb_client_t));
    c_client->user_data = user_data;
    auto fdb_client = new CCClient(name, c_client);
    c_client->native_handle = fdb_client;
    return c_client;
}

fdb_client_t *fdb_client_create_with_handle(void *user_data, void *client_handle)
{
    if (!client_handle)
    {
        return 0;
    }
    auto c_client = new fdb_client_t();
    memset(c_client, 0, sizeof(fdb_client_t));
    c_client->user_data = user_data;
    c_client->native_handle = client_handle;
    return c_client;
}

void *fdb_client_get_user_data(fdb_client_t *handle)
{
    return handle ? handle->user_data : 0;
}

void fdb_client_register_event_handle(fdb_client_t *handle, const fdb_client_handles_t *handles)
{
    if (!handle)
    {
        return;
    }
    handle->handles = handles;
}

void fdb_client_destroy(fdb_client_t *handle)
{
    if (!handle || !handle->native_handle)
    {
        return;
    }
    auto fdb_client = (CCClient *)handle->native_handle;
    fdb_client->prepareDestroy();
    delete fdb_client;
    delete handle;
}

fdb_bool_t fdb_client_connect(fdb_client_t *handle, const char *url, int32_t timeout)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    auto status = fdb_client->connect(url, timeout);
    return status == CONNECTED ? fdb_true : fdb_false;
}

fdb_bool_t fdb_client_disconnect(fdb_client_t *handle)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    fdb_client->disconnect();
    return fdb_true;
}

fdb_bool_t fdb_client_invoke_async(fdb_client_t *handle,
                                   FdbMsgCode_t msg_code,
                                   const uint8_t *msg_data,
                                   int32_t data_size,
                                   int32_t timeout,
                                   enum EFdbQOS qos,
                                   void *user_data,
                                   const char *log_data)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    
    auto fdb_msg = new CCInvokeMsg(msg_code, user_data, qos);
    fdb_msg->setLogData(log_data);
    return fdb_client->invoke(fdb_msg, msg_data, data_size, timeout);
}

fdb_bool_t fdb_client_invoke_callback(fdb_client_t *handle,
                                      FdbMsgCode_t msg_code,
                                      const uint8_t *msg_data,
                                      int32_t data_size,
                                      int32_t timeout,
                                      enum EFdbQOS qos,
                                      fdb_message_reply_fn_t reply_fn,
                                      void *user_data,
                                      const char *log_data)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }

    if (!reply_fn)
    {
        return fdb_client_invoke_async(handle, msg_code, msg_data, data_size,
                                       timeout, qos, user_data, log_data);
    }

    auto fdb_client = (CCClient *)handle->native_handle;
    return fdb_client->invoke(msg_code, [reply_fn, handle, user_data]
            (CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj)
            {
                auto *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
                int32_t error_code = FDB_ST_OK;
                if (fdb_msg->isStatus())
                {
                    std::string reason;
                    if (!fdb_msg->decodeStatus(error_code, reason))
                    {
                        FDB_LOG_E("onReply: fail to decode status!\n");
                        error_code = FDB_ST_MSG_DECODE_FAIL;
                    }
                }

                fdb_message_t reply_msg = {fdb_msg->session(),              //sid
                                           fdb_msg->code(),                 //msg_code
                                           fdb_msg->getPayloadBuffer(),     //msg_data
                                           fdb_msg->getPayloadSize(),       //data_size
                                           error_code,                      //status
                                           0,                               //topic
                                           user_data,                       //user_data
                                           fdb_msg->qos(),                  //qos
                                           0                                //_msg_buffer
                                           };
                reply_fn(handle, &reply_msg);
            },
            (const void *)msg_data, data_size, 0, timeout, qos, log_data
        );
}

fdb_bool_t fdb_client_invoke_sync(fdb_client_t *handle,
                                  FdbMsgCode_t msg_code,
                                  const uint8_t *msg_data,
                                  int32_t data_size,
                                  int32_t timeout,
                                  enum EFdbQOS qos,
                                  const char *log_data,
                                  fdb_message_t *ret_msg)
{
    if (ret_msg)
    {
        ret_msg->status = FDB_ST_UNKNOWN;
        ret_msg->_msg_buffer = 0;
        ret_msg->topic = 0;
    }
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    
    auto fdb_msg = new CBaseMessage(msg_code, qos);
    fdb_msg->setLogData(log_data);
    CBaseJob::Ptr ref(fdb_msg);
    if (!fdb_client->invoke(ref, msg_data, data_size, timeout))
    {
        FDB_LOG_E("fdb_client_invoke_sync: unable to call method\n");
        return fdb_false;
    }

    int32_t error_code = FDB_ST_OK;
    if (fdb_msg->isStatus())
    {
        std::string reason;
        if (!fdb_msg->decodeStatus(error_code, reason))
        {
            FDB_LOG_E("onReply: fail to decode status!\n");
            error_code = FDB_ST_MSG_DECODE_FAIL;
        }
    }

    if (ret_msg)
    {
        ret_msg->sid = fdb_msg->session();
        ret_msg->msg_code = fdb_msg->code();
        ret_msg->msg_data = fdb_msg->getPayloadBuffer();
        ret_msg->data_size = fdb_msg->getPayloadSize();
        ret_msg->status = error_code;
        ret_msg->topic = 0;
        ret_msg->user_data = 0;
        ret_msg->qos = fdb_msg->qos();

        // avoid buffer from being released.
        ret_msg->_msg_buffer = fdb_msg->ownBuffer();
    }

    return fdb_true;
}

void fdb_client_release_return_msg(fdb_message_t *ret_msg)
{
    if (ret_msg)
    {
        if (ret_msg->topic)
        {
            free((void *)(ret_msg->topic));
            ret_msg->topic = 0;
        }
        if (ret_msg->_msg_buffer)
        {
            delete[] (uint8_t *)ret_msg->_msg_buffer;
            ret_msg->_msg_buffer = 0;
        }
    }
}
                                  
fdb_bool_t fdb_client_send(fdb_client_t *handle,
                           FdbMsgCode_t msg_code,
                           const uint8_t *msg_data,
                           int32_t data_size,
                           enum EFdbQOS qos,
                           const char *log_data)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    
    return fdb_client->send(msg_code, msg_data, data_size, qos, log_data);
}

fdb_bool_t fdb_client_subscribe(fdb_client_t *handle,
                                const fdb_subscribe_item_t *sub_items,
                                int32_t nr_items,
                                enum EFdbQOS qos)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;

    if (!nr_items || !sub_items)
    {
        return fdb_false;
    }

    CFdbMsgSubscribeList subscribe_list;
    for (int32_t i = 0; i < nr_items; ++i)
    {
        fdb_client->addNotifyItem(subscribe_list,
                                  sub_items[i].event_code,
                                  sub_items[i].topic);
    }
    return fdb_client->subscribe(subscribe_list, 0, qos);
}

fdb_bool_t fdb_client_unsubscribe(fdb_client_t *handle,
                                  const fdb_subscribe_item_t *sub_items,
                                  int32_t nr_items,
                                  enum EFdbQOS qos)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;

    if (!nr_items || !sub_items)
    {
        return fdb_false;
    }

    CFdbMsgSubscribeList subscribe_list;
    for (int32_t i = 0; i < nr_items; ++i)
    {
        fdb_client->addNotifyItem(subscribe_list,
                                  sub_items[i].event_code,
                                  sub_items[i].topic);
    }
    return fdb_client->unsubscribe(subscribe_list, qos);
}

fdb_bool_t fdb_client_publish(fdb_client_t *handle,
                              FdbMsgCode_t event,
                              const char *topic,
                              const uint8_t *event_data,
                              int32_t data_size,
                              enum EFdbQOS qos,
                              const char *log_data,
                              fdb_bool_t always_update)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    
    return fdb_client->publish(event, event_data, data_size, topic, always_update,
                               qos, log_data);
}

fdb_bool_t fdb_client_get_event_async(fdb_client_t *handle,
                                      FdbMsgCode_t event,
                                      const char *topic,
                                      int32_t timeout,
                                      enum EFdbQOS qos,
                                      void *user_data)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    
    auto fdb_msg = new CCInvokeMsg(event, user_data, qos);
    return fdb_client->get(fdb_msg, topic, timeout);
}

fdb_bool_t fdb_client_get_event_sync(fdb_client_t *handle,
                                     FdbMsgCode_t event,
                                     const char *topic,
                                     int32_t timeout,
                                     enum EFdbQOS qos,
                                     fdb_message_t *ret_msg)
{
    if (ret_msg)
    {
        ret_msg->status = FDB_ST_UNKNOWN;
        ret_msg->_msg_buffer = 0;
        ret_msg->topic = 0;
    }
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }
    
    auto fdb_client = (CCClient *)handle->native_handle;
    
    auto fdb_msg = new CBaseMessage(event, qos);
    CBaseJob::Ptr ref(fdb_msg);
    if (!fdb_client->get(ref, topic, timeout))
    {
        FDB_LOG_E("fdb_client_get_event_sync: unable to call method\n");
        return fdb_false;
    }
    
    int32_t error_code = FDB_ST_OK;
    if (fdb_msg->isStatus())
    {
        std::string reason;
        if (!fdb_msg->decodeStatus(error_code, reason))
        {
            FDB_LOG_E("onReply: fail to decode status!\n");
            error_code = FDB_ST_MSG_DECODE_FAIL;
        }
    }
    
    if (ret_msg)
    {
        ret_msg->sid = fdb_msg->session();
        ret_msg->msg_code = fdb_msg->code();
        ret_msg->msg_data = fdb_msg->getPayloadBuffer();
        ret_msg->data_size = fdb_msg->getPayloadSize();
        ret_msg->status = error_code;
        ret_msg->topic = fdb_msg->topic().empty() ? 0 : fdb_msg->topic().c_str();
        ret_msg->user_data = 0;
        ret_msg->qos = fdb_msg->qos();

        // avoid buffer from being released.
        ret_msg->_msg_buffer = fdb_msg->ownBuffer();
    }
    
    return fdb_true;
}

