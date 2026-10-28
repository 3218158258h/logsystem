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

#ifndef _CNAMEPROXYCONTAINER_H_
#define _CNAMEPROXYCONTAINER_H_

#include <fdbus/CBaseJob.h>
#include <utils/CNsConfig.h>
#include <map>
#include <fdbus/CBaseNameProxy.h>

namespace ipc {
namespace fdbus {

class CInterNameProxy;
class CNameServer;
class FdbMsgServiceTable;
class FdbMsgHostAddressList;

class CNameProxyContainer
{
public:
    CNameProxyContainer(CNameServer *name_server)
        : mNameServer(name_server)
    {}
    void forwardServiceListener(SubscribeType subscribe_type
                              , FdbInstanceId_t instance_id
                              , const char *service_name
                              , CBaseJob::Ptr &msg_ref);
    void recallServiceListener(FdbInstanceId_t instance_id, const char *service_name,
                               SubscribeType subscribe_type);
    CNameServer *nameServer()
    {
        return mNameServer;
    }
    void deleteNameProxy(CInterNameProxy *proxy);
    void addNameProxy(CInterNameProxy *proxy, const char *host_ip);
    void prepareQueryService(tPendingServiceReqTbl *pending_tbl,
                             FdbMsgServiceTable *svc_tbl);
    bool sendQueryServiceReq(CBaseJob::Ptr &msg_ref,
                             tPendingServiceReqTbl *pending_tbl,
                             FdbMsgServiceTable *svc_tbl);

    void getHostTbl(FdbMsgHostAddressList &host_tbl);

protected:
    typedef std::map<std::string, CInterNameProxy *> tNameProxyTbl;
    tNameProxyTbl mNameProxyTbl;
    CNameServer *mNameServer;
};
};
}

#endif
