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

#include "CNameProxyContainer.h"
#include "CInterNameProxy.h"
#include "CNameServer.h"
#include <fdbus/CFdbIfNameServer.h>

namespace ipc {
namespace fdbus {
    
void CNameProxyContainer::forwardServiceListener(SubscribeType subscribe_type
                                               , FdbInstanceId_t instance_id
                                               , const char *service_name
                                               , CBaseJob::Ptr &msg_ref)
{
    for (auto it = mNameProxyTbl.begin(); it != mNameProxyTbl.end(); ++it)
    {
        if (FDB_IP_ALL_INTERFACE == it->first)
        {
            continue;
        }
        auto ns_proxy = it->second;
        if (subscribe_type == INTRA_MONITOR)
        {
            ns_proxy->addServiceMonitorListener(instance_id, service_name, msg_ref);
        }
        else
        {
            ns_proxy->addServiceListener(instance_id, service_name, msg_ref);
        }
    }
}

void CNameProxyContainer::recallServiceListener(FdbInstanceId_t instance_id,
                                                const char *service_name,
                                                SubscribeType subscribe_type)
{
    for (auto it = mNameProxyTbl.begin(); it != mNameProxyTbl.end(); ++it)
    {
        if (FDB_IP_ALL_INTERFACE == it->first)
        {
            continue;
        }
        auto ns_proxy = it->second;
        if (subscribe_type == INTRA_MONITOR)
        {
            ns_proxy->removeServiceMonitorListener(instance_id, service_name);
        }
        else
        {
            ns_proxy->removeServiceListener(instance_id, service_name);
        }
    }
}

void CNameProxyContainer::deleteNameProxy(CInterNameProxy *proxy)
{
    auto it = mNameProxyTbl.find(proxy->getHostIp());
    if (it != mNameProxyTbl.end())
    {
        mNameServer->notifyRemoteNameServerDrop(it->second->getHostName().c_str());
        mNameProxyTbl.erase(it);
    }
}

void CNameProxyContainer::addNameProxy(CInterNameProxy *proxy, const char *host_ip)
{
    mNameProxyTbl[host_ip] = proxy;
}

void CNameProxyContainer::prepareQueryService(tPendingServiceReqTbl *pending_tbl,
                                          FdbMsgServiceTable *svc_tbl)
{
    for (auto np_it = mNameProxyTbl.begin(); np_it != mNameProxyTbl.end(); ++np_it)
    {
        pending_tbl->push_back(np_it->first);
    }
}

bool CNameProxyContainer::sendQueryServiceReq(CBaseJob::Ptr &msg_ref,
                                          tPendingServiceReqTbl *pending_tbl,
                                          FdbMsgServiceTable *svc_tbl)
{
    bool sent = false;
    for (auto np_it = mNameProxyTbl.begin(); np_it != mNameProxyTbl.end(); ++np_it)
    {
        auto req = new CQueryServiceMsg(pending_tbl, svc_tbl, msg_ref, np_it->first, mNameServer);
        if (np_it->second->queryServiceTbl(req))
        {
            sent = true;
        }
    }
    return sent;
}

void CNameProxyContainer::getHostTbl(FdbMsgHostAddressList &host_tbl)
{
    for (auto it = mNameProxyTbl.begin(); it != mNameProxyTbl.end(); ++it)
    {
        auto addr = host_tbl.add_address_list();
        addr->set_ip_address(it->second->getHostIp());
        addr->set_host_name(it->second->getHostName());
        CFdbSocketAddr ns_addr;
        CBaseSocketFactory::parseUrl(it->second->getNsUrl().c_str(), ns_addr);
        auto host_addr = addr->add_address_list();
        host_addr->fromSocketAddress(ns_addr);
        host_addr->set_udp_port(FDB_INET_PORT_INVALID);
    }
}
}
}
