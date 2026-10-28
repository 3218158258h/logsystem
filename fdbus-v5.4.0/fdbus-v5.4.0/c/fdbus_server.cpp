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

#include <string>
#include <fdbus/CBaseServer.h>
#include <fdbus/CFdbMessage.h>
#include <fdbus/CLogProducer.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/fdbus_server.h>
#include <fdbus/CFdbMessage.h>
#define FDB_LOG_TAG "FDB_C"
#include <fdbus/fdb_log_trace.h>
#include <vector>
namespace ipc {
namespace fdbus {

class CCServer : public CBaseServer
{
public:
    CCServer(const char *name, fdb_server_t *server = 0);
    ~CCServer();
protected:
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);
    void onInvoke(CBaseJob::Ptr &msg_ref);
    void onSubscribe(CBaseJob::Ptr &msg_ref);
private:
    fdb_server_t *mServer;
};

CBaseServer *FDB_createCServer(const char *name)
{
    return new CCServer(name);
}

CCServer::CCServer(const char *name, fdb_server_t *server)
    : CBaseServer(name)
    , mServer(server)
{
}

CCServer::~CCServer()
{
}

void CCServer::onOnline(const CFdbOnlineInfo &info)
{
    if (!mServer)
    {
        CFdbBaseObject::onOnline(info);
        return;
    }

    if (!mServer->handles || !mServer->handles->on_online_func)
    {
        return;
    }
    mServer->handles->on_online_func(mServer, info.mSid, info.mFirstOrLast, info.mQOS);
}

void CCServer::onOffline(const CFdbOnlineInfo &info)
{
    if (!mServer)
    {
        CFdbBaseObject::onOffline(info);
        return;
    }

    if (!mServer->handles || !mServer->handles->on_offline_func)
    {
        return;
    }
    mServer->handles->on_offline_func(mServer, info.mSid, info.mFirstOrLast, info.mQOS);
}

void CCServer::onInvoke(CBaseJob::Ptr &msg_ref)
{
    if (!mServer)
    {
        CFdbBaseObject::onInvoke(msg_ref);
        return;
    }

    if (!mServer->handles || !mServer->handles->on_invoke_func)
    {
        return;
    }

    auto fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    if (fdb_msg)
    {
        CBaseJob::Ptr *reply_handle = fdb_msg->needReply() ? new CBaseJob::Ptr(msg_ref) : 0;
        fdb_message_t req_msg = {fdb_msg->session(),              //sid
                                 fdb_msg->code(),                 //msg_code
                                 fdb_msg->getPayloadBuffer(),     //msg_data
                                 fdb_msg->getPayloadSize(),       //data_size
                                 0,                               //status
                                 0,                               //topic
                                 0,                               //user_data
                                 fdb_msg->qos()                   //qos
                                 };
        mServer->handles->on_invoke_func(mServer, &req_msg, reply_handle);
    }
}

void CCServer::onSubscribe(CBaseJob::Ptr &msg_ref)
{
    if (!mServer || !mServer->handles || !mServer->handles->on_subscribe_func)
    {
        return;
    }

    auto fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    std::vector<fdb_subscribe_item_t> event_array;
    std::vector<FdbMsgCode_t> event_code_array;
    std::vector<std::string> topic_array;
    const CFdbMsgSubscribeItem *sub_item;
    FDB_BEGIN_FOREACH_SIGNAL(fdb_msg, sub_item)
    {
        auto msg_code = sub_item->msg_code();
        event_code_array.push_back(msg_code);
        topic_array.push_back(sub_item->topic());
    }
    FDB_END_FOREACH_SIGNAL()
    
    for (uint32_t i = 0; i < event_code_array.size(); ++i)
    {
        event_array.push_back({event_code_array[i], topic_array[i].c_str()});
    }
    
    if (event_array.size())
    {
        auto reply_handle = new CBaseJob::Ptr(msg_ref);
        mServer->handles->on_subscribe_func(mServer,
                                   event_array.data(),
                                   (int32_t)event_array.size(),
                                   reply_handle);
    }
}
}
}
using namespace ipc::fdbus;
fdb_server_t *fdb_server_create(const char *name, void *user_data)
{
    auto c_server = new fdb_server_t();
    memset(c_server, 0, sizeof(fdb_server_t));
    c_server->user_data = user_data;
    auto fdb_server = new CCServer(name, c_server);
    c_server->native_handle = fdb_server;
    return c_server;
}

fdb_server_t *fdb_server_create_with_handle(void *user_data, void *server_handle)
{
    if (!server_handle)
    {
        return 0;
    }
    auto c_server = new fdb_server_t();
    memset(c_server, 0, sizeof(fdb_server_t));
    c_server->user_data = user_data;
    c_server->native_handle = server_handle;
    return c_server;
}

void *fdb_server_get_user_data(fdb_server_t *handle)
{
    return handle ? handle->user_data : 0;
}

void fdb_server_register_event_handle(fdb_server_t *handle, const fdb_server_handles_t *handles)
{
    handle->handles = handles;
}

void fdb_server_destroy(fdb_server_t *handle)
{
    if (!handle || !handle->native_handle)
    {
        return;
    }

    auto fdb_server = (CCServer *)handle->native_handle;
    fdb_server->prepareDestroy();
    delete fdb_server;
    delete handle;
}

fdb_bool_t fdb_server_bind(fdb_server_t *handle, const char *url)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }

    auto fdb_server = (CCServer *)handle->native_handle;
    fdb_server->bind(url);
    return fdb_true;
}

fdb_bool_t fdb_server_unbind(fdb_server_t *handle)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }

    auto fdb_server = (CCServer *)handle->native_handle;
    fdb_server->unbind();
    return fdb_true;
}

fdb_bool_t fdb_server_broadcast(fdb_server_t *handle,
                                FdbMsgCode_t msg_code,
                                const char *topic,
                                const uint8_t *msg_data,
                                int32_t data_size,
                                enum EFdbQOS qos,
                                const char *log_data)
{
    if (!handle || !handle->native_handle)
    {
        return fdb_false;
    }

    auto fdb_server = (CCServer *)handle->native_handle;

    return fdb_server->broadcast(msg_code,
                                 msg_data,
                                 data_size,
                                 topic,
                                 qos,
                                 log_data);
}

fdb_bool_t fdb_message_reply(void *reply_handle,
                             const uint8_t *msg_data,
                             int32_t data_size,
                             const char *log_data)
{
    if (!reply_handle)
    {
        return fdb_false;
    }

    auto msg_ref = (CBaseJob::Ptr *)reply_handle;
    auto msg = castToMessage<CFdbMessage *>(*msg_ref);
    fdb_bool_t ret = fdb_false;
    if (msg)
    {
        ret = msg->reply(*msg_ref, msg_data, data_size, log_data);
    } 

    msg_ref->reset();
    delete msg_ref;

    return ret;
}

fdb_bool_t fdb_message_broadcast(void *reply_handle,
                                 FdbMsgCode_t msg_code,
                                 const char *topic,
                                 const uint8_t *msg_data,
                                 int32_t data_size,
                                 const char *log_data)
{
    if (!reply_handle)
    {
        return fdb_false;
    }

    auto msg_ref = (CBaseJob::Ptr *)reply_handle;
    auto msg = castToMessage<CFdbMessage *>(*msg_ref);
    fdb_bool_t ret = fdb_false;
    if (msg)
    {
        ret = msg->broadcast(msg_code, msg_data, data_size, topic, log_data);
    }

    return ret;
}

void fdb_message_destroy(void *reply_handle)
{
    if (!reply_handle)
    {
        return;
    }

    auto msg_ref = (CBaseJob::Ptr *)reply_handle;
    auto msg = castToMessage<CFdbMessage *>(*msg_ref);
    // why 2? because msg_ref holds another ref count
    if (msg->needReply() && (msg_ref->use_count() <= 2))
    {
        msg->doAutoReply(*msg_ref, FDB_ST_AUTO_REPLY_OK, "Automatically reply to request.");
    }
    msg_ref->reset();

    delete msg_ref;
}

void fdb_server_enable_event_cache(fdb_server_t *handle, fdb_bool_t enable)
{
    if (!handle || !handle->native_handle)
    {
        return;
    }

    auto fdb_server = (CCServer *)handle->native_handle;
    fdb_server->enableEventCache(enable);
}

void fdb_server_init_event_cache(fdb_server_t *handle,
                                 FdbMsgCode_t event,
                                 const char *topic,
                                 const uint8_t *event_data,
                                 int32_t data_size,
                                 fdb_bool_t allow_event_route)
{
    if (!handle || !handle->native_handle)
    {
        return;
    }

    auto fdb_server = (CCServer *)handle->native_handle;
    fdb_server->initEventCache(event, topic, event_data, data_size, allow_event_route);
}


