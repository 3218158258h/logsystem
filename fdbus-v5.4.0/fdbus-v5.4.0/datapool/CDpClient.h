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

#ifndef __CDPCLIENT_H__
#define __CDPCLIENT_H__

#include <map>
#include <vector>
#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbMsgDispatcher.h>
#include <fdbus/dp_common.h>

namespace ipc {
namespace fdbus {

class CDataPool;
class CBaseWorker;
class CFdbBaseContext;
class FdbMsgTopics;

class CDpClient : public CBaseClient
{
public:
    CDpClient(CDataPool *pool, const char *host_name, const char *ep_name,
                     CBaseWorker *handle_worker = 0, CFdbBaseContext *context = 0);
    ~CDpClient();
    bool publishData(CFdbMessage *msg);
    bool subscribeData(FdbMsgCode_t topic_id, const char *topic, std::vector<tDataPoolCallbackFn> &publish_listeners);
    void populateTopicList(FdbMsgTopics &topic_list);
    const std::string &hostName() const
    {
        return mHostName;
    }
    bool topicCreated(const char *topic, FdbMsgCode_t topic_id);
    tTopicTbl &getCreatedTopicList()
    {
        return mTopicTbl;
    }
private:
    class CCtrlChannel : public CFdbBaseObject
    {
    public:
        CCtrlChannel(CDpClient *dp_client);
        void connectWithDpClient();
    protected:
        void onOnline(const CFdbOnlineInfo &info);
        void onBroadcast(CBaseJob::Ptr &msg_ref);
    private:
        CDpClient *mDpClient;
    };
    CCtrlChannel mCtrlChannel;
    CDataPool *mPool;
    tTopicTbl mTopicTbl;
    std::string mHostName;

    void updateTopicList(FdbMsgTopics &topic_list);

    friend class CCtrlChannel;
};


}
}
#endif
