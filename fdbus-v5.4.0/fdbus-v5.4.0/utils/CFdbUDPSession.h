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

#ifndef __CFDBUDPSESSION_H__
#define __CFDBUDPSESSION_H__

#include <string>
#include <fdbus/common_defs.h>
#include <fdbus/CBaseSession.h>

namespace ipc {
namespace fdbus {
struct CFdbSocketAddr;
struct CFdbMsgPrefix;
class CFdbMessageHeader;

class CFdbUDPSession : public CBaseSession
{
public:
    CFdbUDPSession(CFdbSessionContainer *container, CSocketImp *socket);
    ~CFdbUDPSession();

    bool sendMessage(CFdbMessage *msg, tFdbIpV4 peer_ip, int32_t peer_port);

    virtual const std::string &getUDPDestAddr(int32_t &port) const;
    virtual void getUDPDestAddr(tFdbIpV4 &peer_ip, int32_t &peer_port) const;
    virtual void setUDPDestAddr(const char *ip_addr, int32_t port);
    EFdbQOS qos();

    bool sendMessage(CFdbMessage* msg);
    bool sendMessageRef(CBaseJob::Ptr &ref);
protected:
    void onInput();
    void onError();
    void onHup();
};
}
}
#endif
