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

#ifndef __CDPSERVER_H__
#define __CDPSERVER_H__

#include <map>
#include <fdbus/CBaseServer.h>
#include <fdbus/CFdbMsgDispatcher.h>
#include <fdbus/dp_common.h>

namespace ipc {
namespace fdbus {

class CDataPool;
class CBaseWorker;
class CFdbBaseContext;
class FdbMsgTopics;

class CDpServer : public CBaseServer
{
public:
    CDpServer(CDataPool *pool, const char *ep_name, CBaseWorker *handle_worker = 0,
              CFdbBaseContext *context = 0);
    bool publishData(CFdbMessage *msg);
    bool createData(FdbMsgCode_t id,
                    const char *topic,
                    const uint8_t *data,
                    int32_t size,
                    tDataPoolCallbackFn modify_listener);
    void destroyData(FdbMsgCode_t topic_id, const char *topic);
    void populateTopicList(FdbMsgTopics &topic_list);
    bool topicCreated(const char *topic, FdbMsgCode_t topic_id);
private:
    class CCtrlChannel : public CFdbBaseObject
    {
    public:
        CCtrlChannel(CDpServer *dp_server);
        void bindToDpServer();
    protected:
        void onInvoke(CBaseJob::Ptr &msg_ref);
        void onSubscribe(CBaseJob::Ptr &msg_ref);
    private:
        CDpServer *mDpServer;
    };
    CCtrlChannel mCtrlChannel;
    CDataPool *mPool;
    tTopicTbl mTopicTbl;

    friend class CCtrlChannel;
};

}
}

#endif
