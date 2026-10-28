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

#include "CInterHostProxy.h"
#include <utils/Log.h>
#include "CHostServer.h"
#include "CSvcAddrUtils.h"

namespace ipc {
namespace fdbus {
CInterHostProxy::CInterHostProxy(CHostServer *hs, const char *host_url)
    : CBaseHostProxy(host_url)
    , mHostServer(hs)
{
}

void CInterHostProxy::onBroadcast(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (msg->isStatus())
    {
        int32_t id;
        std::string reason;
        msg->decodeStatus(id, reason);
        LOG_I("CIntraHostProxy: onBroadcast(): status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());
        return;
    }
    switch (msg->code())
    {
        case NTF_EXPORT_SVC_ADDRESS_EXTERNAL:
        {
            FdbMsgExportableSvcAddress svc_address_tbl;
            CFdbParcelableParser parser(svc_address_tbl);
            if (!msg->deserialize(parser))
            {
                return;
            }
            CSvcAddrUtils::validateUrl(svc_address_tbl, msg->getSession());
            mHostServer->updateUpstreamSvcTable(mSvcAddrTbl, svc_address_tbl);
            break;
        }
        default:
            CBaseHostProxy::onBroadcast(msg_ref);
        break;
    }
}

void CInterHostProxy::onOnline(const CFdbOnlineInfo &info)
{
    CBaseHostProxy::onOnline(info);

    FdbMsgDomainRegisterReq domain_req;
    domain_req.set_domain_name(mHostServer->name());
    // TODO set cred!!!
    CFdbParcelableBuilder builder(domain_req);
    invoke(REQ_REGISTER_EXTERNAL_DOMAIN, builder);

    LOG_I("CInterHostProxy: connected to host server: %s\n.", mHostUrl.c_str());
}

void CInterHostProxy::onOffline(const CFdbOnlineInfo &info)
{
    CBaseHostProxy::onOffline(info);
    // clear service table and propagand to related domains
    // TODO: should we do this???
    FdbMsgExportableSvcAddress svc_address_tbl;
    mHostServer->updateUpstreamSvcTable(mSvcAddrTbl, svc_address_tbl);
}

void CInterHostProxy::onReply(CBaseJob::Ptr &msg_ref)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (msg->isStatus())
    {
        int32_t id;
        std::string reason;
        msg->decodeStatus(id, reason);
        LOG_I("CInterHostProxy: onReply(): status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());
        return;
    }

    switch (msg->code())
    {
        case REQ_REGISTER_EXTERNAL_DOMAIN:
        {
            FdbMsgHostRegisterAck ack;
            CFdbParcelableParser parser(ack);
            if (!msg->deserialize(parser))
            {
                return;
            }
            if (ack.has_token_list() && mHostServer->importTokens(ack.token_list().tokens()))
            {
                mHostServer->updateSecurityLevel();
                LOG_I("CIntraHostProxy: tokens of name server is updated.\n");
            }
            mDomainName = ack.domain_name();

            FdbMsgDomainAddress domain_addr;
            domain_addr.set_domain_name(mHostServer->name());
            mHostServer->buildAddrTblForUpstreamHost(domain_addr.export_svc_address());

            CFdbParcelableBuilder builder(domain_addr);
            send(REQ_EXTERNAL_HOST_READY, builder);
            {
                CFdbMsgSubscribeList subscribe_list;
                addNotifyItem(subscribe_list, NTF_EXPORT_SVC_ADDRESS_EXTERNAL);
                subscribe(subscribe_list, 0);
            }
        }
    }
}

void CInterHostProxy::postSvcTable(FdbMsgExportableSvcAddress &svc_address_tbl)
{
    CFdbParcelableBuilder builder(svc_address_tbl);
    if (connected())
    {
        send(REQ_POST_DOMAIN_SVC_ADDRESS, builder);
    }
}
}
}
