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

#include <fdbus/CEventSubscribeHandle.h>
#include <fdbus/CFdbSession.h>
#include <fdbus/CFdbMessage.h>

namespace ipc {
namespace fdbus {
void CEventSubscribeHandle::subscribe(CFdbSession *session,
                               FdbMsgCode_t msg,
                               FdbObjectId_t obj_id,
                               const char *topic,
                               uint32_t flag)
{
    if (!topic)
    {
        topic = "";
    }
    auto &subitem = mEventSubscribeTable[msg][session][obj_id][topic];
    subitem.mFlag = flag;
}

void CEventSubscribeHandle::unsubscribe(CFdbSession *session,
                                 FdbMsgCode_t msg,
                                 FdbObjectId_t obj_id,
                                 const char *topic)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;
    
    auto it_sessions = subscribe_table.find(msg);
    if (it_sessions != subscribe_table.end())
    {
        auto &sessions = it_sessions->second;
        auto it_objects = sessions.find(session);
        if (it_objects != sessions.end())
        {
            auto &objects = it_objects->second;
            auto it_subitems = objects.find(obj_id);
            if (it_subitems != objects.end())
            {
                if (topic)
                {
                    auto &subitems = it_subitems->second;
                    auto it_subitem = subitems.find(topic);
                    if (it_subitem != subitems.end())
                    {
                        subitems.erase(it_subitem);
                    }
                    if (subitems.empty())
                    {
                        objects.erase(it_subitems);
                    }
                }
                else
                {
                    objects.erase(it_subitems);
                }
            }
            if (objects.empty())
            {
                sessions.erase(it_objects);
            }
        }
        if (sessions.empty())
        {
            subscribe_table.erase(it_sessions);
        }
    }
}

void CEventSubscribeHandle::unsubscribe(CFdbSession *session)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;

    for (auto it_sessions = subscribe_table.begin();
            it_sessions != subscribe_table.end();)
    {
        auto the_it_sessions = it_sessions;
        ++it_sessions;

        auto &sessions = the_it_sessions->second;
        auto it_objects = sessions.find(session);
        if (it_objects != sessions.end())
        {
            sessions.erase(it_objects);
        }
        if (sessions.empty())
        {
            subscribe_table.erase(the_it_sessions);
        }
    }
}

void CEventSubscribeHandle::unsubscribe(FdbObjectId_t obj_id)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;

    for (auto it_sessions = subscribe_table.begin();
            it_sessions != subscribe_table.end();)
    {
        auto the_it_sessions = it_sessions;
        ++it_sessions;

        auto &sessions = the_it_sessions->second;
        for (auto it_objects = sessions.begin();
                it_objects != sessions.end();)
        {
            auto the_it_objects = it_objects;
            ++it_objects;

            auto &objects = the_it_objects->second;
            auto it_subitems = objects.find(obj_id);
            if (it_subitems != objects.end())
            {
                objects.erase(it_subitems);
            }
            if (objects.empty())
            {
                sessions.erase(the_it_objects);
            }
        }
        if (sessions.empty())
        {
            subscribe_table.erase(the_it_sessions);
        }
    }
}

bool CEventSubscribeHandle::broadcastOneMsg(CFdbSession *session,
                                             CFdbMessage *msg,
                                             CSubscribeItem &sub_item,
                                             FdbSessionId_t sender_sid,
                                             FdbObjectId_t sender_oid)
{
    // do not broadcast to the sender publishing the message. This is
    // the case: client publish an event meanwhile it subscribed the
    // same event. To avoid the event being received by itself, the
    // client shall assert FDB_SUBFLAG_NO_REFLECT flag when subscribing
    // the event.
    if ( msg->isNoReflect() && (sub_item.mFlag & FDB_SUBFLAG_NO_REFLECT) &&
            (sender_sid == session->sid()) && (sender_oid == msg->objectId()))
    {
        return false;
    }
    return session->sendMessage(msg);
}

bool CEventSubscribeHandle::broadcast(CFdbMessage *msg, FdbMsgCode_t event)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;
    auto sender_sid = msg->session();
    auto sender_oid = msg->objectId();

    bool sent = false;
    auto it_sessions = subscribe_table.find(event);
    if (it_sessions != subscribe_table.end())
    {
        auto topic = msg->topic().c_str();
        auto &sessions = it_sessions->second;
        for (auto it_objects = sessions.begin();
                it_objects != sessions.end(); ++it_objects)
        {
            auto session = it_objects->first;
            auto &objects = it_objects->second;
            for (auto it_subitems = objects.begin();
                    it_subitems != objects.end(); ++it_subitems)
            {
                auto object_id = it_subitems->first;
                msg->updateObjectId(object_id); // send to the specific object.
                auto &subitems = it_subitems->second;
                auto it_subitem = subitems.find(topic);
                if (it_subitem != subitems.end())
                {
                    broadcastOneMsg(session, msg, it_subitem->second, sender_sid, sender_oid);
                    sent = true;
                }
                /*
                 * If topic doesn't match, check who registers topic "".
                 * It represents any topic.
                 */
                if (topic[0] != '\0')
                {
                    auto it_subitem = subitems.find("");
                    if (it_subitem != subitems.end())
                    {
                        broadcastOneMsg(session, msg, it_subitem->second, sender_sid, sender_oid);
                        sent = true;
                    }
                }
            }
        }
    }

    return sent;
}

bool CEventSubscribeHandle::broadcast(CFdbMessage *msg, CFdbSession *session, FdbMsgCode_t event)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;
    auto sender_sid = msg->session();
    auto sender_oid = msg->objectId();

    bool sent = false;
    auto it_sessions = subscribe_table.find(event);
    if (it_sessions != subscribe_table.end())
    {
        auto &sessions = it_sessions->second;
        auto it_objects = sessions.find(session);
        if (it_objects != sessions.end())
        {
            auto &objects = it_objects->second;
            for (auto it_subitems = objects.begin(); it_subitems != objects.end(); ++it_subitems)
            {
                if (FDB_OBJECT_GET_CLASS(it_subitems->first) != FDB_OBJECT_GET_CLASS(msg->objectId()))
                {
                    continue;
                }

                msg->updateObjectId(it_subitems->first); // send to the specific object.
                auto topic = msg->topic().c_str();
                auto &subitems = it_subitems->second;
                auto it_subitem = subitems.find(topic);
                if (it_subitem != subitems.end())
                {
                    broadcastOneMsg(session, msg, it_subitem->second, sender_sid, sender_oid);
                    sent = true;
                }
                else if (topic[0] != '\0')
                {
                    auto it_subitem = subitems.find("");
                    if (it_subitem != subitems.end())
                    {
                        broadcastOneMsg(session, msg, it_subitem->second, sender_sid, sender_oid);
                        sent = true;
                    }
                }
            }
        }
    }
    return sent;
}

void CEventSubscribeHandle::getSubscribeTable(SessionTable_t &sessions, tFdbFilterSets &topic_tbl)
{
    for (auto it_objects = sessions.begin(); it_objects != sessions.end(); ++it_objects)
    {
        auto &objects = it_objects->second;
        for (auto it_subitems = objects.begin(); it_subitems != objects.end(); ++it_subitems)
        {
            auto &subitems = it_subitems->second;
            for (auto it_subitem = subitems.begin(); it_subitem != subitems.end(); ++it_subitem)
            {
                topic_tbl.insert(it_subitem->first);
            }
        }
    }
}

void CEventSubscribeHandle::getSubscribeTable(tFdbSubscribeMsgTbl &table)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;

    for (auto it_sessions = subscribe_table.begin();
            it_sessions != subscribe_table.end(); ++it_sessions)
    {
        auto &topic_table = table[it_sessions->first];
        auto &sessions = it_sessions->second;
        getSubscribeTable(sessions, topic_table);
    }
}

void CEventSubscribeHandle::getSubscribeTable(FdbMsgCode_t code, tFdbFilterSets &topics)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;

    auto it_sessions = subscribe_table.find(code);
    if (it_sessions != subscribe_table.end())
    {
        auto &sessions = it_sessions->second;
        getSubscribeTable(sessions, topics);
    }
}

void CEventSubscribeHandle::getSubscribeTable(FdbMsgCode_t code, CFdbSession *session,
                                              tFdbFilterSets &topic_tbl)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;

    auto it_sessions = subscribe_table.find(code);
    if (it_sessions != subscribe_table.end())
    {
        auto &sessions = it_sessions->second;
        auto it_objects = sessions.find(session);
        if (it_objects != sessions.end())
        {
            auto &objects = it_objects->second;
            for (auto it_subitems = objects.begin();
                    it_subitems != objects.end(); ++it_subitems)
            {
                auto &subitems = it_subitems->second;
                for (auto it_subitem = subitems.begin(); it_subitem != subitems.end(); ++it_subitem)
                {
                    topic_tbl.insert(it_subitem->first);
                }
            }
        }
    }
}

void CEventSubscribeHandle::getSubscribeTable(FdbMsgCode_t code, const char *topic,
                                              tSubscribedSessionSets &session_tbl)
{
    SubscribeTable_t &subscribe_table = mEventSubscribeTable;

    auto it_sessions = subscribe_table.find(code);
    if (it_sessions != subscribe_table.end())
    {
        if (!topic)
        {
            topic = "";
        }
        auto &sessions = it_sessions->second;
        for (auto it_objects = sessions.begin();
                it_objects != sessions.end(); ++it_objects)
        {
            auto session = it_objects->first;
            auto &objects = it_objects->second;
            for (auto it_subitems = objects.begin();
                    it_subitems != objects.end(); ++it_subitems)
            {
                auto &subitems = it_subitems->second;
                auto it_subitem = subitems.find(topic);
                if (it_subitem == subitems.end())
                {
                    /*
                     * If topic doesn't match, check who registers topic "".
                     * It represents any topic.
                     */
                    if (topic[0] != '\0')
                    {
                        auto it_subitem = subitems.find("");
                        if (it_subitem != subitems.end())
                        {
                            session_tbl.insert(session);
                            break;
                        }
                    }
                }
                else
                {
                    session_tbl.insert(session);
                    break;
                }
            }
        }
    }
                                              }
                                              }
}
