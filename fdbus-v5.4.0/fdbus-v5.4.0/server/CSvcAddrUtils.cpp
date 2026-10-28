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

#include "CSvcAddrUtils.h"
#include <fdbus/CFdbIfNameServer.h>
#include <fdbus/CFdbSession.h>
#include <fdbus/CBaseSocketFactory.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
CFdbInterfaceTable CSvcAddrUtils::mInterfaceTable;

void CSvcAddrUtils::setServiceInfo(const CFdbSvcAddress &exported_addr,
                                   FdbMsgAddressList &export_addr_list)
{
    export_addr_list.set_service_name(exported_addr.mSvcName);
    export_addr_list.set_endpoint_name(exported_addr.mEndpointName);
    export_addr_list.set_host_name(exported_addr.mHostName);
    export_addr_list.set_domain_name(exported_addr.mDomainName);
    export_addr_list.set_pid(0);
    export_addr_list.set_is_local(false);
    export_addr_list.populateTokens(exported_addr.mTokens);
    export_addr_list.set_instance_id(exported_addr.mInstanceId);
}

bool CSvcAddrUtils::getExportable(int32_t exportable, int32_t &level, bool &at_least)
{
    if (exportable < 0)
    {
        at_least = true;
        level = -exportable;
    }
    else if (exportable == 0)
    {
        LOG_E("CSvcAddrUtils: exportable level 0 is not allowed!\n");
        return false;
    }
    else
    {
        at_least = false;
        level = exportable;
    }
    return true;
}

bool CSvcAddrUtils::checkExportable(int32_t current, int32_t desired, bool at_least)
{
    if (at_least)
    {
        if (current < desired)
        {
            return false;
        }
    }
    else
    {
        if (current > desired)
        {
            return false;
        }
    }

    return true;
}

void CSvcAddrUtils::setAddressItem(const CFdbSocketAddrInfo &exported_item,
                                   FdbMsgAddressItem *msg_item)
{
    msg_item->fromSocketAddress(exported_item.mAddress);
    msg_item->set_udp_port(exported_item.mUDPPort);
    msg_item->set_exportable_level(exported_item.mExportableLevel);
    msg_item->set_interface_id(exported_item.mInterfaceId);
}

void CSvcAddrUtils::populateFromHostInfo(const CFdbSvcAddress &exported_addr,
                                         FdbMsgAddressList &export_addr_list,
                                         int32_t exportable_level, bool remove)
{
    setServiceInfo(exported_addr, export_addr_list);

    if (!remove)
    {
        bool at_least;
        int32_t level;
        if (!getExportable(exportable_level, level, at_least))
        {
            return;
        }
        for (auto it = exported_addr.mAddrTbl.begin(); it != exported_addr.mAddrTbl.end(); ++it)
        {
            if (!checkExportable(it->mExportableLevel, level, at_least))
            {
                continue;
            }
  
            setAddressItem(*it, export_addr_list.add_address_list());
        }
    }
}

void CSvcAddrUtils::populateFromHostInfo(const tSvcAddrDescTbl &svc_addr_tbl,
                                         FdbMsgExportableSvcAddress &svc_address_tbl,
                                         int32_t exportable_level)
{
    for (auto svc_it = svc_addr_tbl.begin(); svc_it != svc_addr_tbl.end(); ++svc_it)
    {
        auto &exported_addr = *svc_it;
        FdbMsgAddressList *export_addr_list = 0;
        bool at_least;
        int32_t level;
        if (!getExportable(exportable_level, level, at_least))
        {
            return;
        }
        for (auto it = exported_addr.mAddrTbl.begin(); it != exported_addr.mAddrTbl.end(); ++it)
        {
            if (!checkExportable(it->mExportableLevel, level, at_least))
            {
                continue;
            }

            if (!export_addr_list)
            {
                export_addr_list = svc_address_tbl.add_svc_address_list();
                setServiceInfo(exported_addr, *export_addr_list);
            }
            
            setAddressItem(*it, export_addr_list->add_address_list());
        }
    }
}

void CSvcAddrUtils::populateToHostInfo(FdbMsgExportableSvcAddress &svc_address_tbl,
                                     tSvcAddrDescTbl &svc_addr_tbl)
{
    auto &addr_tbl = svc_address_tbl.svc_address_list();
    for (auto svc_it = addr_tbl.vpool().begin(); svc_it != addr_tbl.vpool().end(); ++svc_it)
    {
        auto &addr_list = *svc_it;
        svc_addr_tbl.resize(svc_addr_tbl.size() + 1);
        auto &exported_addr = svc_addr_tbl.back();
        populateToHostInfo(addr_list, exported_addr);
    }
}

void CSvcAddrUtils::populateToHostInfo(FdbMsgAddressList &addr_list, CFdbSvcAddress &exported_addr)
{
    if (addr_list.address_list().vpool().empty())
    {
        return;
    }
    exported_addr.mSvcName = addr_list.service_name();
    exported_addr.mEndpointName = addr_list.endpoint_name();
    exported_addr.mHostName = addr_list.host_name();
    exported_addr.mDomainName = addr_list.domain_name();
    exported_addr.mInstanceId = addr_list.instance_id();
    addr_list.dumpTokens(exported_addr.mTokens);

    for (auto addr_it = addr_list.address_list().vpool().begin();
         addr_it != addr_list.address_list().vpool().end(); ++addr_it)
    {
        auto &addr_item = *addr_it;
        exported_addr.mAddrTbl.resize(exported_addr.mAddrTbl.size() + 1);
        auto &sock_addr = exported_addr.mAddrTbl.back();
        addr_item.toSocketAddress(sock_addr.mAddress);
        sock_addr.mUDPPort = addr_item.udp_port();
        sock_addr.mExportableLevel = addr_item.exportable_level();
        sock_addr.mInterfaceId = addr_item.interface_id();
    }
}

void CSvcAddrUtils::compareSvcTable(FdbMsgExportableSvcAddress &new_address_tbl,
                                    tSvcAddrDescTbl &current_svc_tbl,
                                    tSvcAddrDescTbl &added_svc_tbl,
                                    tSvcAddrDescTbl &removed_svc_tbl)
{
    // if address of the service is in new_address_tbl but not in current svc list,
    // it is regarded as new
    auto &addr_tbl = new_address_tbl.svc_address_list();
    for (auto addr_it = addr_tbl.vpool().begin(); addr_it != addr_tbl.vpool().end(); ++addr_it)
    {
        auto &addr_list = *addr_it;
        bool found = false;
        for (auto svc_it = current_svc_tbl.begin(); svc_it != current_svc_tbl.end(); ++svc_it)
        {
            if ((svc_it->mSvcName == addr_list.service_name()) &&
                (svc_it->mInstanceId == addr_list.instance_id()))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            added_svc_tbl.resize(added_svc_tbl.size() + 1);
            auto &exported_addr = added_svc_tbl.back();
            populateToHostInfo(addr_list, exported_addr);
        }
    }

    // if a service is in current svc list but its address is not in new_address_tbl,
    // it is regarded as removed
    for (auto svc_it = current_svc_tbl.begin(); svc_it != current_svc_tbl.end(); ++svc_it)
    {
        auto &exported_addr = *svc_it;
        bool found = false;
        for (auto addr_it = addr_tbl.vpool().begin(); addr_it != addr_tbl.vpool().end(); ++addr_it)
        {
            if ((addr_it->service_name() == exported_addr.mSvcName) &&
                (addr_it->instance_id() == exported_addr.mInstanceId))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            removed_svc_tbl.resize(removed_svc_tbl.size() + 1);
            auto &removed_addr = removed_svc_tbl.back();
            removed_addr = exported_addr;
        }
    }
}

void CSvcAddrUtils::printExternalSvc(const CFdbSvcAddress &svc, const char *prefix)
{
    LOG_I("%ssvc: %s, endpoint: %s, host: %s, domain: %s, token:\n", prefix,
          svc.mSvcName.c_str(), svc.mEndpointName.c_str(), svc.mHostName.c_str(), svc.mDomainName.c_str());
    for (auto it = svc.mTokens.begin(); it != svc.mTokens.end(); ++it)
    {
        LOG_I("%s    %s\n", prefix, it->c_str());
    }
    for (auto it = svc.mAddrTbl.begin(); it != svc.mAddrTbl.end(); ++it)
    {
        LOG_I("%s        url: %s, udp_port: %d, exportable: %d, id: %d\n",
              prefix, it->mAddress.mUrl.c_str(), it->mUDPPort, it->mExportableLevel, it->mInterfaceId);
    }
}

void CSvcAddrUtils::printExternalSvcTbl(const tSvcAddrDescTbl &svc_tbl, const char *prefix)
{
    for (auto it = svc_tbl.begin(); it != svc_tbl.end(); ++it)
    {
        printExternalSvc(*it, prefix);
    }
}

void CSvcAddrUtils::removeDuplicateService(tSvcAddrDescTbl &original, const tSvcAddrDescTbl &duplicate)
{
    for (auto orig_it = original.begin(); orig_it != original.end();)
    {
        auto &orig_svc = *orig_it;
        auto the_it = orig_it;
        ++orig_it;
        for (auto dup_it = duplicate.begin(); dup_it != duplicate.end(); ++dup_it)
        {
            bool same_service = false;
            auto dup_svc = *dup_it;
            if (orig_svc.mSvcName == dup_svc.mSvcName)
            {
                for (auto orig_sock_it = orig_svc.mAddrTbl.begin(); orig_sock_it != orig_svc.mAddrTbl.end(); ++orig_sock_it)
                {
                    for (auto dup_sock_it = dup_svc.mAddrTbl.begin(); dup_sock_it != dup_svc.mAddrTbl.end(); ++dup_sock_it)
                    {
                        if (dup_sock_it->mAddress.mAddr == FDB_IP_ALL_INTERFACE)
                        {
                            if (mInterfaceTable.mAddrTbl.empty())
                            {
                                CBaseSocketFactory::getIpAddress(mInterfaceTable);
                            }
                            if (mInterfaceTable.findByIp(orig_sock_it->mAddress.mAddr.c_str()))
                            {
                                if (orig_sock_it->mAddress.mPort == dup_sock_it->mAddress.mPort)
                                {
                                    same_service = true;
                                    break;
                                }
                            }
                        }
                        else if (orig_sock_it->mAddress.mUrl == dup_sock_it->mAddress.mUrl)
                        {   // if any address in list equal, the two services are regarded as the same
                            same_service = true;
                            break;
                        }
                    }
                    if (same_service)
                    {
                        break;
                    }
                }
            }
            if (same_service)
            {
                original.erase(the_it);
                break;
            }
        }
    }
}

void CSvcAddrUtils::validateUrl(FdbMsgAddressList &msg_addr_list, CFdbSession *session)
{
    // empty means dropping, so should broadcast anyway
    if (!session || msg_addr_list.address_list().pool().empty())
    {
        return;
    }

    // make sure 1) UDS and FDB_LOCAL_HOST is not broadcasted;
    //           2) FDB_IP_ALL_INTERFACE is replaced with peer address
    //           3) order of interface:
    //              identical to peer -> all interface (should be replaced) -> others
    std::string peer_ip;
    session->peerIp(peer_ip);

    CFdbParcelableArray<FdbMsgAddressItem>::tPool &pool =
                                    msg_addr_list.address_list().vpool();
    CFdbParcelableArray<FdbMsgAddressItem> best_candidates;
    CFdbParcelableArray<FdbMsgAddressItem> all_if_candidates;
    CFdbParcelableArray<FdbMsgAddressItem> fallback_candidates;
    for (auto it = pool.begin(); it != pool.end(); ++it)
    {
        if ((it->address_type() == FDB_SOCKET_IPC) || (it->tcp_ipc_address() == FDB_LOCAL_HOST))
        {
        }
        else if (it->tcp_ipc_address() == peer_ip)
        {
            auto item = best_candidates.Add();
            *item = *it;
        }
        else if ((it->tcp_ipc_address() == FDB_IP_ALL_INTERFACE) && !peer_ip.empty())
        {
            auto item = all_if_candidates.Add();
            *item = *it;
            item->set_tcp_ipc_address(peer_ip);
            CBaseSocketFactory::buildUrl(item->tcp_ipc_url(), peer_ip.c_str(),
                                         it->tcp_port(), it->is_secure());
        }
        else
        {
            auto item = fallback_candidates.Add();
            *item = *it;
        }
    }
    if (!best_candidates.vpool().empty())
    {
        msg_addr_list.address_list().vpool() = best_candidates.vpool();
    }
    else if (!all_if_candidates.vpool().empty())
    {
        msg_addr_list.address_list().vpool() = all_if_candidates.vpool();
    }
    else if (!fallback_candidates.vpool().empty())
    {
        msg_addr_list.address_list().vpool() = fallback_candidates.vpool();
    }
    else
    {
        LOG_E("CSvcAddrUtils: unable to broadcast address for service %s!\n",
              msg_addr_list.service_name().c_str());
    }
}

void CSvcAddrUtils::validateUrl(FdbMsgExportableSvcAddress &msg_addr_tbl, CFdbSession *session)
{
    auto &addr_tbl = msg_addr_tbl.svc_address_list();
    for (auto svc_it = addr_tbl.vpool().begin(); svc_it != addr_tbl.vpool().end(); ++svc_it)
    {
        validateUrl(*svc_it, session);
    }
}

void CSvcAddrUtils::verifyUrl(CFdbParcelableArray<FdbMsgAddressItem> &addr_pool,
                                CFdbSession *session)
{
    if (!session || addr_pool.pool().empty())
    {
        return;
    }

    std::string peer_ip;
    session->peerIp(peer_ip);
    if (peer_ip == FDB_LOCAL_HOST)
    {
        return;
    }

    for (auto it = addr_pool.vpool().begin(); it < addr_pool.vpool().end();)
    {
        if ((it->address_type() == FDB_SOCKET_IPC) || (it->tcp_ipc_address() == FDB_LOCAL_HOST))
        {
            it = addr_pool.vpool().erase(it);
        }
        else
        {
            if ((it->tcp_ipc_address() == FDB_IP_ALL_INTERFACE) && !peer_ip.empty())
            {
                it->set_tcp_ipc_address(peer_ip);
                CBaseSocketFactory::buildUrl(it->tcp_ipc_url(), peer_ip.c_str(),
                                             it->tcp_port(), it->is_secure());
            }
            ++it;
        }
    }
}

void CSvcAddrUtils::verifyUrl(FdbMsgExportableSvcAddress &msg_addr_tbl, CFdbSession *session)
{
    auto &addr_tbl = msg_addr_tbl.svc_address_list();
    for (auto svc_it = addr_tbl.vpool().begin(); svc_it != addr_tbl.vpool().end(); ++svc_it)
    {
        verifyUrl(svc_it->address_list(), session);
    }
}
}
}
