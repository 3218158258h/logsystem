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

#include <fdbus/CFdbAFComponent.h>
#include <fdbus/CMethodJob.h>
#include <fdbus/CBaseClient.h>
#include <fdbus/CBaseServer.h>
#include <fdbus/CFdbContext.h>

namespace ipc {
namespace fdbus {
CFdbAFComponent::CFdbAFComponent(const char *name, CBaseWorker *worker, CFdbBaseContext *context)
    : mName(name ? name : "Unknown-Component")
    , mWorker(worker)
{
    mContext = context ? context : FDB_CONTEXT;
}

CBaseClient *CFdbAFComponent::findClient(const char *bus_name)
{
    if (!bus_name)
    {
        return 0;
    }
    auto it = mClientTbl.find(bus_name);
    if (it == mClientTbl.end())
    {
        return 0;
    }
    return it->second;
}

bool CFdbAFComponent::registerClient(const char *bus_name, CBaseClient *client)
{
    std::string url(FDB_URL_SVC);
    url += bus_name;
    client->connect(url.c_str());
    mClientTbl[bus_name] = client;
    return true;
}

CBaseServer *CFdbAFComponent::findService(const char *bus_name)
{
    if (!bus_name)
    {
        return 0;
    }
    auto it = mServerTbl.find(bus_name);
    if (it == mServerTbl.end())
    {
        return 0;
    }
    return it->second;
}
bool CFdbAFComponent::registerService(const char *bus_name, CBaseServer *server)
{
    std::string url(FDB_URL_SVC);
    url += bus_name;
    server->bind(url.c_str());
    mServerTbl[bus_name] = server;
    return true;
}

class CQueryServiceJob : public CMethodJob<CFdbAFComponent>
{
public:
    CQueryServiceJob(CFdbAFComponent *comp,
                     const char *bus_name,
                     const CEvtHandleTbl &evt_tbl,
                     CFdbBaseObject::tConnCallbackFn &connect_callback,
                     CBaseClient *&client,
                     CBaseWorker *worker)
        : CMethodJob<CFdbAFComponent>(comp, &CFdbAFComponent::callQueryService, JOB_FORCE_RUN)
        , mBusName(bus_name)
        , mEvtTbl(evt_tbl)
        , mConnCallback(connect_callback)
        , mClient(client)
        , mWorker(worker)
    {
    }
    const char *mBusName;
    const CEvtHandleTbl &mEvtTbl;
    CFdbBaseObject::tConnCallbackFn &mConnCallback;
    CBaseClient *&mClient;
    CBaseWorker *mWorker;
};

CBaseClient *CFdbAFComponent::createClient(CBaseWorker *worker)
{
    return new CBaseClient(mName.c_str(), worker, mContext);
}

void CFdbAFComponent::callQueryService(CMethodJob<CFdbAFComponent> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CQueryServiceJob *>(job);
    auto client = findClient(the_job->mBusName);
    if (!client)
    {
        client = createClient(the_job->mWorker ? the_job->mWorker : mWorker);
        if (!client)
        {
            the_job->mClient = 0;
            return;
        }
        registerClient(the_job->mBusName, client);
    }
    auto handle = client->doRegisterConnNotification(the_job->mConnCallback, mWorker);
    mConnHandleTbl.push_back(handle);
    client->doRegisterEventHandle(the_job->mEvtTbl, &mEventRegistryTbl);
    the_job->mClient = client;
}

CBaseClient *CFdbAFComponent::queryService(const char *bus_name,
                                            const CEvtHandleTbl &evt_tbl,
                                            CFdbBaseObject::tConnCallbackFn connect_callback,
                                            CBaseWorker *worker)
{
    if (!bus_name)
    {
        return 0;
    }
    CBaseClient *client = 0;
    auto job = new CQueryServiceJob(this, bus_name, evt_tbl, connect_callback, client, worker);
    mContext->sendSyncEndeavor(job);
    return client;
}

CBaseClient *CFdbAFComponent::queryService(const char *bus_name,
                                            CFdbBaseObject::tConnCallbackFn connect_callback,
                                            CBaseWorker *worker)
{
    CEvtHandleTbl dummy_evt_tbl;
    return queryService(bus_name, dummy_evt_tbl, connect_callback, worker);
}

bool CFdbAFComponent::addEvtHandle(CEvtHandleTbl &hdl_table, FdbMsgCode_t code,
                                   tDispatcherCallbackFn callback, const char *topic)
{
    return hdl_table.add(code, callback, mWorker, topic);
}

bool CFdbAFComponent::invoke(CFdbBaseObject *obj
                             , FdbMsgCode_t code
                             , IFdbMsgBuilder &data
                             , CFdbBaseObject::tInvokeCallbackFn callback
                             , int32_t timeout
                             , enum EFdbQOS qos)
{
    return obj->invoke(code, data, callback, mWorker, timeout, qos);
}

bool CFdbAFComponent::invoke(CFdbBaseObject *obj
                             , FdbMsgCode_t code
                             , CFdbBaseObject::tInvokeCallbackFn callback
                             , const void *buffer
                             , int32_t size
                             , int32_t timeout
                             , enum EFdbQOS qos
                             , const char *log_info)
{
    return obj->invoke(code, callback, buffer, size, mWorker, timeout, qos, log_info);
}

class COfferServiceJob : public CMethodJob<CFdbAFComponent>
{
public:
    COfferServiceJob(CFdbAFComponent *comp,
                     const char *bus_name,
                     const CMsgHandleTbl &msg_tbl,
                     CFdbBaseObject::tConnCallbackFn &connect_callback,
                     CBaseServer *&server,
                     CBaseWorker *worker)
        : CMethodJob<CFdbAFComponent>(comp, &CFdbAFComponent::callOfferService, JOB_FORCE_RUN)
        , mBusName(bus_name)
        , mMsgTbl(msg_tbl)
        , mConnCallback(connect_callback)
        , mServer(server)
        , mWorker(worker)
    {
    }
    const char *mBusName;
    const CMsgHandleTbl &mMsgTbl;
    CFdbBaseObject::tConnCallbackFn &mConnCallback;
    CBaseServer *&mServer;
    CBaseWorker *mWorker;
};

CBaseServer *CFdbAFComponent::createServer(CBaseWorker *worker)
{
    return new CBaseServer(mName.c_str(), worker, mContext);
}

void CFdbAFComponent::callOfferService(CMethodJob<CFdbAFComponent> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<COfferServiceJob *>(job);
    auto server = findService(the_job->mBusName);
    if (!server)
    {
        server = createServer(the_job->mWorker ? the_job->mWorker : mWorker);
        if (!server)
        {
            the_job->mServer = 0;
            return;
        }
        server->enableEventCache(true);
        registerService(the_job->mBusName, server);
    }
    auto handle = server->doRegisterConnNotification(the_job->mConnCallback, mWorker);
    mConnHandleTbl.push_back(handle);
    server->doRegisterMsgHandle(the_job->mMsgTbl);
    the_job->mServer = server;
}

CBaseServer *CFdbAFComponent::offerService(const char *bus_name,
                                            const CMsgHandleTbl &msg_tbl,
                                            CFdbBaseObject::tConnCallbackFn connect_callback,
                                            CBaseWorker *worker)
{
    if (!bus_name)
    {
        return 0;
    }
    CBaseServer *server = 0;
    auto job = new COfferServiceJob(this, bus_name, msg_tbl, connect_callback, server, worker);
    mContext->sendSyncEndeavor(job);
    return server;
}

CBaseServer *CFdbAFComponent::offerService(const char *bus_name,
                                            CFdbBaseObject::tConnCallbackFn connect_callback,
                                            CBaseWorker *worker)
{
    CMsgHandleTbl dummy_msg_tbl;
    return offerService(bus_name, dummy_msg_tbl, connect_callback, worker);
}

bool CFdbAFComponent::addMsgHandle(CMsgHandleTbl &hdl_table, FdbMsgCode_t code,
                                   tDispatcherCallbackFn callback)
{
    return hdl_table.add(code, callback, mWorker);
                                   }
                                   }
}
