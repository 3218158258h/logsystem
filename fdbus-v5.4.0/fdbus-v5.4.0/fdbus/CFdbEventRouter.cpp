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

#include <fdbus/CFdbEventRouter.h>
#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CFdbSession.h>

namespace ipc {
namespace fdbus {
class CEventRouterProxy : public CBaseClient
{
public:
    CEventRouterProxy(const char *peer_name, CFdbEventRouter *router)
        : mRouter(router)
        , mPeerName(peer_name ? peer_name : "")
    {
        enableReconnect(true);
    }
    void connectToPeer()
    {
        if (!name().empty())
        {
            return;
        }

        name(mRouter->endpoint()->nsName().c_str());
        std::string peer_url(FDB_URL_SVC);
        peer_url += mPeerName;
        connect(peer_url.c_str());
    }
protected:
    void onOnline(const CFdbOnlineInfo &info);
private:
    CFdbEventRouter *mRouter;
    std::string mPeerName;
};

void CEventRouterProxy::onOnline(const CFdbOnlineInfo &info)
{
    if (info.mFirstOrLast)
    {
        mRouter->syncEventPool(getSession(info.mSid));
    }
}

CFdbEventRouter::~CFdbEventRouter()
{
    for (auto it = mPeerTbl.begin(); it != mPeerTbl.end(); ++it)
    {
        (*it)->prepareDestroy();
        delete *it;
    }
}

void CFdbEventRouter::addPeer(const char *peer_name)
{
    auto peer = new CEventRouterProxy(peer_name, this);
    mPeerTbl.push_back(peer);
}

void CFdbEventRouter::connectPeers()
{
    for (auto it = mPeerTbl.begin(); it != mPeerTbl.end(); ++it)
    {
        (*it)->connectToPeer();
    }
}

void CFdbEventRouter::routeMessage(CFdbSession *session, CFdbMessage *msg)
{
    for (auto it = mPeerTbl.begin(); it != mPeerTbl.end(); ++it)
    {
        /* avoid back and forth between NCs */
        if (session->senderName().compare((*it)->nsName()))
        {
            (*it)->publishNoQueue(msg->code()
                                  , msg->topic().c_str()
                                  , msg->getPayloadBuffer()
                                  , msg->getPayloadSize()
                                  , 0
                                  , msg->isForceUpdate()
                                  , msg->qos());
        }
    }
}

void CFdbEventRouter::syncEventPool(CFdbSession *session)
{
    if (session)
    {
        mEndpoint->publishCachedEvents(session);
    }
}
}
}
