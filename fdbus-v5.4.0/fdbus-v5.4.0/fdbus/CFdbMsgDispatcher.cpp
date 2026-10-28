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

#include <algorithm>
#include <utils/Log.h>
#include <fdbus/CFdbMsgDispatcher.h>
#include <fdbus/CFdbBaseObject.h>

namespace ipc {
namespace fdbus {
bool CFdbMsgDispatcher::registerCallback(const CMsgHandleTbl &msg_tbl)
{
    bool ret = true;
    for (auto it = msg_tbl.mTable.begin(); it != msg_tbl.mTable.end(); ++it)
    {
        auto code = it->mCode;
        if (mRegistryTbl.find(code) != mRegistryTbl.end())
        {
            LOG_E("CFdbMsgDispatcher: handle for method %d is aready registered.\n", code);
            ret = false;
            continue;
        }
        mRegistryTbl[code] = *it;
    }
    return ret;
}

bool CFdbMsgDispatcher::processMessage(CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    tRegistryTbl::iterator it = mRegistryTbl.find(msg->code());
    if ((it == mRegistryTbl.end()) || !it->second.mCallback)
    {
        return false;
    }
    auto worker = it->second.mWorker;
    if (!worker)
    {
        worker = obj->worker();
    }
    if (!worker || worker->isSelf())
    {
        try // catch exception to avoid missing post processing
        {
            it->second.mCallback(msg_ref, obj);
        }
        catch (...)
        {
            LOG_E("CFdbMsgDispatcher: except is caught at processMessage!\n");
        }
    }
    else
    {
        tJobCallable inner = std::bind(it->second.mCallback, std::placeholders::_1, obj);
        tJobCallable outer = std::bind(&CFdbMessage::callbackWithAutoReply, msg,
                                                   std::placeholders::_1, inner);
        msg->setCallable(outer);
        worker->sendAsync(msg_ref);
    }
    return true;
}

bool CMsgHandleTbl::add(FdbMsgCode_t code, tDispatcherCallbackFn callback,
                        CBaseWorker *worker)
{
    if (!callback)
    {
        return false;
    }
    mTable.resize(mTable.size() + 1);
    auto &item = mTable.back();
    item.mCode = code;
    item.mCallback = callback;
    item.mWorker = worker;
    return true;
}

bool CEvtHandleTbl::add(FdbMsgCode_t code, tDispatcherCallbackFn callback,
                        CBaseWorker *worker, const char *topic, uint32_t flag)
{
    if (!callback)
    {
        return false;
    }
    mTable.resize(mTable.size() + 1);
    auto &item = mTable.back();
    item.mCode = code;
    item.mCallback = callback;
    if (topic)
    {
        item.mTopic = topic;
    }
    item.mWorker = worker;
    item.mFlag = flag;
    return true;
}

bool CEvtHandleTbl::addGroup(FdbMsgCode_t group, tDispatcherCallbackFn callback,
                             CBaseWorker *worker, const char *topic, uint32_t flag)
{
    return add(fdbMakeGroupEvent(group), callback, worker, topic, flag);
}

bool CBaseEventDispatcher::processMessage(CBaseJob::Ptr &msg_ref,
                                         FdbMsgCode_t code,
                                         CFdbBaseObject *obj,
                                         tEvtHandlePtrTbl &handles_to_invoke,
                                         const tRegistryHandleTbl *registered_evt_tbl)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    auto it_topics = mRegistryTbl.find(code);
    if (it_topics != mRegistryTbl.end())
    {
        auto &topics = it_topics->second;
        const char *match_topics[] = {msg->topic().c_str(), ""};
        for (int32_t i = 0; i < Fdb_Num_Elems(match_topics); ++i)
        {
            auto it_callbacks = topics.find(match_topics[i]);
            if (it_callbacks == topics.end())
            {
                continue;
            }

            auto &callbacks = it_callbacks->second;
            if (registered_evt_tbl && !registered_evt_tbl->empty())
            {
                for (auto it_callback = callbacks.begin(); it_callback != callbacks.end(); ++it_callback)
                {
                    auto reg_id = it_callback->first;
                    auto it_reg_id = std::find(registered_evt_tbl->begin(), registered_evt_tbl->end(), reg_id);
                    if (it_reg_id != registered_evt_tbl->end())
                    {
                        handles_to_invoke.push_back(&(it_callback->second));
                    }
                }
            }
            else
            {
                for (auto it_callback = callbacks.begin(); it_callback != callbacks.end(); ++it_callback)
                {
                    handles_to_invoke.push_back(&(it_callback->second));
                }
            }
        }
    }
    return true;
}

void CBaseEventDispatcher::dispatchEvents(tEvtHandlePtrTbl &handles_to_invoke,
                                         CBaseJob::Ptr &msg_ref, CFdbMessage *msg,
                                         CFdbBaseObject *obj)
{
    if (handles_to_invoke.empty())
    {
        return;
    }
    auto handle = handles_to_invoke.front();
    if (handles_to_invoke.size() == 1)
    {
        migrateCallback(msg_ref, msg, handle->mCallback, handle->mWorker, obj);
        return;
    }

    auto next_msg = new CFdbMessage(msg);
    CFdbMessage *cur_msg;
    migrateCallback(msg_ref, msg, handle->mCallback, handle->mWorker, obj);
    for (auto it_callback = handles_to_invoke.begin() + 1; it_callback != handles_to_invoke.end();)
    {
        cur_msg = next_msg;
        auto cur_it = it_callback++;
        if (it_callback != handles_to_invoke.end())
        {
            next_msg = new CFdbMessage(msg);
        }
        CBaseJob::Ptr cur_msg_ref(cur_msg);
        migrateCallback(cur_msg_ref, cur_msg, (*cur_it)->mCallback, (*cur_it)->mWorker, obj);
    }
}


bool CBaseEventDispatcher::registerCallback(const CEvtHandleTbl &evt_tbl,
                                            tRegistryHandleTbl *registered_evt_tbl)
{
    for (auto it = evt_tbl.mTable.begin(); it != evt_tbl.mTable.end(); ++it)
    {
        auto code = it->mCode;
        auto &topic = it->mTopic;
        tRegEntryId id = mRegIdAllocator++;
        mRegistryTbl[code][topic][id] = *it;
        if (registered_evt_tbl)
        {
            registered_evt_tbl->push_back(id);
        }
    }
    return true;
}

void CBaseEventDispatcher::dumpEvents(tEvtHandleTbl &event_table)
{
    for (auto it_topics = mRegistryTbl.begin(); it_topics != mRegistryTbl.end(); ++it_topics)
    {
        FdbMsgCode_t code = it_topics->first;
        auto &topics = it_topics->second;
        for (auto it_callbacks = topics.begin(); it_callbacks != topics.end(); ++it_callbacks)
        {
            auto &regids = it_callbacks->second;
            auto it_regid = regids.begin();
            if (it_regid != regids.end())
            {
                const char *topic = it_callbacks->first.c_str();
                event_table.resize(event_table.size() + 1);
                auto &item = event_table.back();
                item.mCode = code;
                item.mTopic = topic;
                // flag is obtained from the first subscription!!!
                item.mFlag = it_regid->second.mFlag;
            }
        }
    }
}

void CBaseEventDispatcher::dumpEventDetails(tEvtHandleTbl &event_table)
{
    for (auto it_topics = mRegistryTbl.begin(); it_topics != mRegistryTbl.end(); ++it_topics)
    {
        auto &topics = it_topics->second;
        for (auto it_callbacks = topics.begin(); it_callbacks != topics.end(); ++it_callbacks)
        {
            auto &regids = it_callbacks->second;
            for (auto it_regid = regids.begin(); it_regid != regids.end(); ++it_regid)
            {
                event_table.resize(event_table.size() + 1);
                auto &item = event_table.back();
                item = it_regid->second;
            }
        }
    }
}

void CFdbSetEvtDispatcher::migrateCallback(CBaseJob::Ptr &msg_ref,
                                           CFdbMessage *msg,
                                           tDispatcherCallbackFn &fn,
                                           CBaseWorker *worker,
                                           CFdbBaseObject *obj)
{
    if (!fn)
    {
        return;
    }
    if (!worker)
    {
        worker = obj->worker();
    }
    if (!worker || worker->isSelf())
    {
        if (fn)
        {
            try // catch exception to avoid missing post processing
            {
                fn(msg_ref, obj);
            }
            catch (...)
            {
                LOG_E("migrateCallback: except is caught!\n");
            }
        }
    }
    else
    {
        tJobCallable inner = std::bind(fn, std::placeholders::_1, obj);
        tJobCallable outer = std::bind(&CFdbMessage::callbackWithAutoReply, msg,
                                                   std::placeholders::_1, inner);
        msg->setCallable(outer);
        worker->sendAsync(msg_ref);
    }
}

void CFdbEventDispatcher::migrateCallback(CBaseJob::Ptr &msg_ref,
                                          CFdbMessage *msg,
                                          tDispatcherCallbackFn &fn,
                                          CBaseWorker *worker,
                                          CFdbBaseObject *obj)
{
    fdbMigrateCallback(msg_ref, msg, fn, worker, obj);
}

void fdbMigrateCallback(CBaseJob::Ptr &msg_ref, CFdbMessage *msg, tDispatcherCallbackFn &fn,
                               CBaseWorker *worker, CFdbBaseObject *obj)
{
    if (!fn)
    {
        return;
    }
    if (!worker || worker->isSelf())
    {
        if (fn)
        {
            try // catch exception to avoid missing post processing
            {
                fn(msg_ref, obj);
            }
            catch (...)
            {
                LOG_E("fdbMigrateCallback: except is caught!\n");
            }
        }
    }
    else
    {
        msg->setCallable(std::bind(fn, std::placeholders::_1, obj));
        worker->sendAsync(msg_ref);
    }
}
}
}
