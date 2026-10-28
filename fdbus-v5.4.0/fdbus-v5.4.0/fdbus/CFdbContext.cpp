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
#include "CIntraNameProxy.h"
#include <fdbus/CLogProducer.h>
#include <utils/Log.h>
#ifdef CONFIG_SSL
#include <openssl/ssl.h>
#endif

namespace ipc {
namespace fdbus {
#define FDB_DEFAULT_LOG_CACHE_SIZE  8 * 1024 * 1024
bool CFdbContext::mEnableNameProxy = true;
bool CFdbContext::mEnableLogger = true;
bool CFdbContext::mEnableLogCache = true;
int32_t CFdbContext::mLogCacheSize = 0;
CFdbContext *CFdbContext::mInstance = 0;
std::mutex CFdbContext::mInstanceMutex;

CFdbContext::CFdbContext()
    : CFdbBaseContext("FDBusDefaultContext")
    , mNameProxy(0)
    , mLogger(0)
{
#ifdef CONFIG_SSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#endif
    mInstance = this;
    mInstanceMutex.unlock();
    // now mInstance is valid and CFdbContext::getInstance() works.

    if (mEnableNameProxy)
    {
        auto name_proxy = new CIntraNameProxy();
        name_proxy->connectToNameServer();
        mNameProxy = name_proxy;
    }
    if (mEnableLogger)
    {
        int32_t cache_size = 0;
        if (mEnableLogCache)
        {
            cache_size = mLogCacheSize ? mLogCacheSize : FDB_DEFAULT_LOG_CACHE_SIZE;
        }
        auto logger = new CLogProducer(cache_size);
        std::string svc_url;
        logger->getDefaultSvcUrl(svc_url);
        logger->doConnect(svc_url.c_str());
        mLogger = logger;
    }
}

CFdbContext *CFdbContext::getInstance()
{
    if (!mInstance)
    {
        mInstanceMutex.lock();
        // In constructor:
        // mInstance will be assigned;
        // mInstanceMutex will be unlocked.
        if (!mInstance)
        {
            new CFdbContext();
        }
    }
    return mInstance;
}

const char *CFdbContext::getFdbLibVersion()
{
    return FDB_DEF_TO_STR(FDB_VERSION_MAJOR) "." FDB_DEF_TO_STR(FDB_VERSION_MINOR) "." FDB_DEF_TO_STR(FDB_VERSION_BUILD);
}

bool CFdbContext::destroy()
{
    if (mNameProxy)
    {
        auto name_proxy = mNameProxy;
        mNameProxy = 0;
        name_proxy->enableNsMonitor(false);
        name_proxy->prepareDestroy();
        delete name_proxy;
    }
    if (mLogger)
    {
        auto logger = mLogger;
        mLogger = 0;
        logger->prepareDestroy();
        delete logger;
    }

    exit();
    join();
    delete this;
    return true;
}

CIntraNameProxy *CFdbContext::getNameProxy()
{
    return mNameProxy;
}

CLogProducer *CFdbContext::getLogger()
{
    return mLogger;
}

void CFdbContext::registerNsWatchdogListener(tNsWatchdogListenerFn watchdog_listener)
{
    if (mNameProxy)
    {
        mNameProxy->registerNsWatchdogListener(watchdog_listener);
    }
}

class CRegisterContextJob : public CMethodJob<CFdbContext>
{
public:
    CRegisterContextJob(CFdbContext *object, CFdbBaseContext *context, bool do_register)
        : CMethodJob<CFdbContext>(object, &CFdbContext::callRegisterContext, JOB_FORCE_RUN)
        , mContext(context)
        , mDoRegister(do_register)
    {}
    CFdbBaseContext *mContext;
    bool mDoRegister;
};

void CFdbContext::callRegisterContext(CMethodJob<CFdbContext> *job, CBaseJob::Ptr &ref)
{
    auto the_job = fdb_dynamic_cast_if_available<CRegisterContextJob *>(job);
    if (the_job->mDoRegister)
    {
        doRegisterContext(the_job->mContext);
    }
    else
    {
        doUnregisterContext(the_job->mContext);
    }
}

void CFdbContext::doRegisterContext(CFdbBaseContext *context)
{
    auto id = mContextContainer.allocateEntityId();
    context->ctxId(id);
    mContextContainer.insertEntry(id, context);
}

void CFdbContext::doUnregisterContext(CFdbBaseContext *context)
{
    mContextContainer.deleteEntry(context->ctxId());
}

void CFdbContext::registerContext(CFdbBaseContext *context)
{
    sendSyncEndeavor(new CRegisterContextJob(this, context, true));
}

void CFdbContext::unregisterContext(CFdbBaseContext *context)
{
    sendSyncEndeavor(new CRegisterContextJob(this, context, false));
}

CFdbBaseContext *CFdbContext::getContext(FdbContextId_t ctx_id)
{
    CFdbBaseContext *context = 0;
    (void)mContextContainer.retrieveEntry(ctx_id, context);
    return context;
}
}
}
