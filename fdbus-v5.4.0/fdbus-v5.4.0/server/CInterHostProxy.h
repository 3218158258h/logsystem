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

#ifndef _CINTERHOSTPROXY_H_
#define _CINTERHOSTPROXY_H_

#include "CBaseHostProxy.h"
#include "CSvcAddrUtils.h"

namespace ipc {
namespace fdbus {
class CHostServer;

class CInterHostProxy : public CBaseHostProxy
{
public:
    CInterHostProxy(CHostServer *hs, const char *host_url);
    CInterHostProxy();
    tSvcAddrDescTbl &getSvcTable()
    {
        return mSvcAddrTbl;
    }
    void postSvcTable(FdbMsgExportableSvcAddress &svc_address_tbl);
protected:
    void onBroadcast(CBaseJob::Ptr &msg_ref);
    void onReply(CBaseJob::Ptr &msg_ref);
    void onOnline(const CFdbOnlineInfo &info);
    void onOffline(const CFdbOnlineInfo &info);
private:
    CHostServer *mHostServer;
    tSvcAddrDescTbl mSvcAddrTbl;
};
}
}
#endif
