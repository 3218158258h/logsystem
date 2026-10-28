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

#include <fdbus/CFdbBaseContext.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CBaseEndpoint.h>
#include <fdbus/CFdbSession.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
CFdbBaseContext::CFdbBaseContext(const char *worker_name, uint32_t flag)
    : CBaseWorker(worker_name, flag | FDB_WORKER_ENABLE_FD_LOOP)
    , mCtxId(FDB_INVALID_ID)
    , mHoseKeepingTimer(this)
{
}

CFdbBaseContext *CFdbBaseContext::create(const char *worker_name, uint32_t flag)
{
    auto ctx = new CFdbBaseContext(worker_name, flag);
    ctx->start(flag);
    return ctx;
}

bool CFdbBaseContext::startHouseKeeping(uint32_t flag, int32_t housekeeping_interval)
{
    auto ret = start(flag);
    if (ret)
    {
        enableHosekeeping(housekeeping_interval);
    }
    return ret;
}

bool CFdbBaseContext::tearup()
{
    FDB_CONTEXT->registerContext(this);
    return true;
}

void CFdbBaseContext::teardown()
{
    mHoseKeepingTimer.attach(0);
    FDB_CONTEXT->unregisterContext(this);
}

CFdbBaseContext::~CFdbBaseContext()
{
    if (!mEndpointContainer.getContainer().empty())
    {
    }
}

CBaseEndpoint *CFdbBaseContext::getEndpoint(FdbEndpointId_t endpoint_id)
{
    CBaseEndpoint *endpoint = 0;
    mEndpointContainer.retrieveEntry(endpoint_id, endpoint);
    return endpoint;
}

FdbEndpointId_t CFdbBaseContext::doRegisterEndpoint(CBaseEndpoint *endpoint)
{
    auto id = endpoint->epid();
    if (fdbValidFdbId(id))
    {
        if (mEndpointContainer.findEntry(endpoint))
        {
            return id;
        }
    }
    else
    {
#if defined(CONFIG_CREATE_EP_ASYNC)
        {
            std::lock_guard<std::mutex> _l(mContainerMutex);
            id = mEndpointContainer.allocateEntityId();
        }
#else
        id = mEndpointContainer.allocateEntityId();
#endif
        endpoint->epid(id);
    }

    mEndpointContainer.insertEntry(id, endpoint);
    endpoint->enableMigrate(true);
    return id;
}

void CFdbBaseContext::doUnregisterEndpoint(CBaseEndpoint *endpoint)
{
    CBaseEndpoint *self = 0;
    auto it = mEndpointContainer.retrieveEntry(endpoint->epid(), self);
    if (self)
    {
        endpoint->enableMigrate(false);
        endpoint->epid(FDB_INVALID_ID);
        mEndpointContainer.deleteEntry(it);
    }
}

class CRegisterJob : public CMethodJob<CFdbBaseContext>
{
public:
    CRegisterJob(CFdbBaseContext *object, CBaseEndpoint *endpoint, bool do_register)
        : CMethodJob<CFdbBaseContext>(object, &CFdbBaseContext::callRegisterEndpoint, JOB_FORCE_RUN)
        , mEndpoint(endpoint)
        , mDoRegister(do_register)
    {
    }

    CBaseEndpoint *mEndpoint;
    bool mDoRegister;
};

void CFdbBaseContext::callRegisterEndpoint(CMethodJob<CFdbBaseContext> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CRegisterJob *>(job);
    if (the_job)
    {
        if (the_job->mDoRegister)
        {
            doRegisterEndpoint(the_job->mEndpoint);
        }
        else
        {
            doUnregisterEndpoint(the_job->mEndpoint);
        }
    }
}

FdbEndpointId_t CFdbBaseContext::registerEndpoint(CBaseEndpoint *endpoint)
{
    auto id = endpoint->epid();
    if (fdbValidFdbId(id))
    {
        return id;
    }
#if defined(CONFIG_CREATE_EP_ASYNC)
    {
        std::lock_guard<std::mutex> _l(mContainerMutex);
        id = mEndpointContainer.allocateEntityId();
    }
    endpoint->epid(id);
    sendAsyncEndeavor(new CRegisterJob(this, endpoint, true));
#else
    sendSyncEndeavor(new CRegisterJob(this, endpoint, true));
    id = endpoint->epid(); // retrieve the id since it is created at the sync call
#endif
    return id;
}

void CFdbBaseContext::unregisterEndpoint(CBaseEndpoint *endpoint)
{
    sendSyncEndeavor(new CRegisterJob(this, endpoint, false));
}

bool CFdbBaseContext::serverAlreadyRegistered(CBaseEndpoint *endpoint)
{
    auto &container = mEndpointContainer.getContainer();
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        auto instance_id = it->second->instanceId();
        if ((it->second->role() == FDB_OBJECT_ROLE_SERVER) &&
            !it->second->nsName().compare(endpoint->nsName()) &&
            (!fdbIsGroup(instance_id) && (instance_id == endpoint->instanceId())) &&
            (it->second != endpoint))
        {
            return true;
        }
    }

    return false;
}

void CFdbBaseContext::regularHouseKeeping(CMethodLoopTimer<CFdbBaseContext> *timer)
{
    auto &container = mEndpointContainer.getContainer();
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        it->second->regularHouseKeeping();
    }
}

void CFdbBaseContext::enableHosekeeping(int32_t housekeeping_interval)
{
    if (housekeeping_interval < 0)
    {
        mHoseKeepingTimer.disable();
        return;
    }

    if (!housekeeping_interval)
    {
        housekeeping_interval = FDB_HOUSEKEEPING_INTRVAL;
    }
    mHoseKeepingTimer.attach(this);
    mHoseKeepingTimer.enable(housekeeping_interval);
}
}
}
