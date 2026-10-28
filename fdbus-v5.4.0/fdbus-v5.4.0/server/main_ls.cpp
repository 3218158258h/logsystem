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

#include <string>
#include <fdbus/CFdbContext.h>
#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbIfNameServer.h>
#include <iostream>
#include <stdlib.h>
#include <fdbus/fdb_option_parser.h>
#include <utils/Log.h>
#include <fdbus/CFdbSimpleMsgBuilder.h>
#include <fdbus/CBaseNameProxy.h>

using namespace ipc::fdbus;

static int32_t ls_verbose = 0;
static int32_t ls_follow = 0;
static int32_t ls_exportable_service = 0;
static std::string ls_host_of_ns;
static std::string ls_service_name;
static FdbInstanceId_t ls_instance_id = fdbMakeGroupEvent(FDB_EVENT_ALL_GROUPS);

class CNameServerProxy : public CBaseClient
{
public:
    CNameServerProxy();
    ~CNameServerProxy()
    {
        disconnect();
    }
protected:
    bool connectionEnabled(const CConnectionInfo &conn_info)
    {
        if (ls_host_of_ns.empty())
        {
            return conn_info.mIsLocal;
        }
        return *conn_info.mHostName == ls_host_of_ns;
    }
    void onReply(CBaseJob::Ptr &msg_ref)
    {
        auto msg = castToMessage<CFdbMessage *>(msg_ref);
        if (msg->isStatus())
        {
            if (msg->isError())
            {
                int32_t id;
                std::string reason;
                msg->decodeStatus(id, reason);
                LOG_I("CNameServerProxy: status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());
                quit();
            }
            return;
        }
        
        switch (msg->code())
        {
            case REQ_QUERY_SERVICE:
            {
                FdbMsgServiceTable svc_tbl;
                CFdbParcelableParser parser(svc_tbl);
                if (!msg->deserialize(parser))
                {
                    LOG_E("CNameServerProxy: unable to decode FdbMsgServiceTable.\n");
                    quit();
                }
                const char *prev_ip = "";
                const char *prev_host = "";
                auto &svc_list = svc_tbl.service_tbl();
                for (auto svc_it = svc_list.vpool().begin(); svc_it != svc_list.vpool().end(); ++svc_it)
                {
                    auto &host_addr = svc_it->host_addr();
                    auto &service_addr = svc_it->service_addr();
                    const char *location = service_addr.is_local() ? "(local)" : "(remote)";

                    if (host_addr.ip_address().compare(prev_ip) || host_addr.host_name().compare(prev_host))
                    {
                        std::cout << "[" << host_addr.host_name() << location << "]"
                                  << " - IP: " << host_addr.ip_address()
                                  << std::endl;
                        prev_ip = host_addr.ip_address().c_str();
                        prev_host = host_addr.host_name().c_str();
                    }
                    std::cout << "    [" << service_addr.service_name()
                              << ":" << (uint32_t)service_addr.instance_id()
                              << "(0x" << std::hex << (uint32_t)service_addr.instance_id() << ")" << std::dec
                              << " (" << service_addr.endpoint_name() << "), pid: "
                              << service_addr.pid() << "]" << std::endl;
                    auto &addr_list = service_addr.address_list();
                    for (auto addr_it = addr_list.vpool().begin();
                            addr_it != addr_list.vpool().end(); ++addr_it)

                    {
                        if (addr_it->has_udp_port() && FDB_VALID_PORT(addr_it->udp_port()))
                        {
                            std::cout << "        > " << addr_it->tcp_ipc_url()
                                      << " udp://" << addr_it->udp_port()
                                      << ", exportable: " << addr_it->exportable_level()
                                      << ", id: " << addr_it->interface_id()
                                      << std::endl;
                        }
                        else
                        {
                            std::cout << "        > " << addr_it->tcp_ipc_url()
                                      << ", exportable: " << addr_it->exportable_level()
                                      << ", id: " << addr_it->interface_id()
                                      << std::endl;
                        }
                    }
                }

                printExportableSvcTable(svc_tbl.exported_service_tbl(), "");
            }
            break;
            case REQ_NS_QUERY_EXPORTABLE_SERVICE:
            {
                FdbMsgAllExportableService exp_service;
                CFdbParcelableParser parser(exp_service);
                if (!msg->deserialize(parser))
                {
                    LOG_E("CNameServerProxy: unable to decode FdbMsgAllExportableService.\n");
                    quit();
                }
                std::cout << "[domain(local) services]" << std::endl;
                printExportableSvcTable(exp_service.local_table(), "    ");
                std::cout << "[upstream services]" << std::endl;
                printExportableSvcTable(exp_service.upstream_table(), "    ");
                std::cout << "[downstream services]" << std::endl;
                printExportableSvcTable(exp_service.downstream_table(), "    ");
            }
            break;
            default:
            break;
        }
        if (!ls_follow)
        {
            quit();
        }
    }

    void onOnline(const CFdbOnlineInfo &info)
    {
        if (!info.mFirstOrLast)
        {
            return;
        }
        if (ls_exportable_service)
        {
            ls_follow = false;
            invoke(REQ_NS_QUERY_EXPORTABLE_SERVICE, (const void *)0, 0, 0, info.mQOS);
        }
        else if (!ls_follow)
        {
            invoke(REQ_QUERY_SERVICE, (const void *)0, 0, 0, info.mQOS);
        }
    }
    
private:
    class ConnectAddrSubscriber : public CFdbBaseObject
    {
    public:
        ConnectAddrSubscriber(CNameServerProxy *ns_proxy)
            : mNsProxy(ns_proxy)
        {}
        void connectWithProxy()
        {
            connect(mNsProxy, INTRA_MONITOR);
        }
    protected:
        void onOnline(const CFdbOnlineInfo &info)
        {
            if (!ls_exportable_service && ls_follow)
            {
                CFdbMsgSubscribeList subscribe_list;
                //addNotifyGroup(subscribe_list, FDB_EVENT_ALL_GROUPS);
                subscribe_list.addNotifyItem(ls_instance_id, ls_service_name.c_str());
                subscribe(subscribe_list, 0, info.mQOS);
            }
        }
        void onBroadcast(CBaseJob::Ptr &msg_ref)
        {
            auto msg = castToMessage<CFdbMessage *>(msg_ref);
            FdbMsgAddressList msg_addr_list;
            CFdbParcelableParser parser(msg_addr_list);
            if (!msg->deserialize(parser))
            {
                LOG_E("CNameServerProxy: unable to decode FdbMsgAddressList.\n");
                return;
            }
            const char *location = msg_addr_list.is_local() ? "(local)" : "(remote)";
            if (msg_addr_list.address_list().empty())
            {
                std::cout << "[" << msg_addr_list.service_name()
                          << ":" << (uint32_t)msg_addr_list.instance_id()
                          << "(0x" << std::hex << (uint32_t)msg_addr_list.instance_id() << ")" << std::dec
                          << " (" << msg_addr_list.endpoint_name()
                          << ")]@" << msg_addr_list.host_name() << location
                          << " - Dropped" << std::endl;
            }
            else
            {
                std::cout << "[" << msg_addr_list.service_name()
                          << ":" << (uint32_t)msg_addr_list.instance_id()
                          << "(0x" << std::hex << (uint32_t)msg_addr_list.instance_id() << ")" << std::dec
                          << " (" << msg_addr_list.endpoint_name()
                          << ")]@" << msg_addr_list.host_name() << location
                          << " - Online" << std::endl;
                auto &addr_list = msg_addr_list.address_list();
                for (auto it = addr_list.vpool().begin(); it != addr_list.vpool().end(); ++it)
                {
                    if (it->has_udp_port() && FDB_VALID_PORT(it->udp_port()))
                    {
                        std::cout << "    > " << it->tcp_ipc_url()
                                  << " udp://" << it->udp_port()
                                  << ", exportable: " << it->exportable_level()
                                  << ", id: " << it->interface_id()
                                  << std::endl;
                    }
                    else
                    {
                        std::cout << "    > " << it->tcp_ipc_url()
                                  << ", exportable: " << it->exportable_level()
                                  << ", id: " << it->interface_id()
                                  << std::endl;
                    }
                }
            }

            if (ls_verbose)
            {
                mNsProxy->invoke(REQ_QUERY_SERVICE);
            }
        }
    private:
        CNameServerProxy *mNsProxy;
    };
    void quit()
    {
        exit(0);
    }
    void printExportableSvcTable(FdbMsgExportableSvcAddress &exp_service, const char *prefix)
    {
        auto &addr_list_tbl = exp_service.svc_address_list();
        std::string prev_host_name;
        for (auto svc_it = addr_list_tbl.vpool().begin(); svc_it != addr_list_tbl.vpool().end(); ++svc_it)
        {
            auto &service_addr = *svc_it;
            if (prev_host_name != service_addr.host_name())
            {
                std::cout << prefix << "[" << service_addr.host_name() << " (public)" << "]"
                          << std::endl;
                prev_host_name = service_addr.host_name();
            }
            std::cout << prefix << "    [" << service_addr.service_name()
                      << ":" << (uint32_t)service_addr.instance_id()
                      << "(0x" << std::hex << (uint32_t)service_addr.instance_id() << ")" << std::dec
                      << " (" << service_addr.endpoint_name() << ")]" << std::endl;
            auto &addr_list = service_addr.address_list();
            for (auto addr_it = addr_list.vpool().begin();
                    addr_it != addr_list.vpool().end(); ++addr_it)
            {
                if (addr_it->has_udp_port() && FDB_VALID_PORT(addr_it->udp_port()))
                {
                    std::cout << prefix << "        > " << addr_it->tcp_ipc_url()
                              << " udp://" << addr_it->udp_port()
                              << ", exportable: " << addr_it->exportable_level()
                              << ", id: " << addr_it->interface_id()
                              << std::endl;
                }
                else
                {
                    std::cout << prefix << "        > " << addr_it->tcp_ipc_url()
                              << ", exportable: " << addr_it->exportable_level()
                              << ", id: " << addr_it->interface_id()
                              << std::endl;
                }
            }
        }
    }

    ConnectAddrSubscriber mIntraMonitorSubscriber;
};

CNameServerProxy::CNameServerProxy()
    : CBaseClient(FDB_NAME_SERVER_NAME)
    , mIntraMonitorSubscriber(this)
{
    mIntraMonitorSubscriber.connectWithProxy();
}

int main(int argc, char **argv)
{
    int32_t help = 0;
    char *host = 0;
    char *service_name = 0;
    char *instance_id = 0;
    const struct fdb_option core_options[] = {
        { FDB_OPTION_BOOLEAN, "follow", 'f', &ls_follow },
        { FDB_OPTION_BOOLEAN, "verbose", 'v', &ls_verbose },
        { FDB_OPTION_BOOLEAN, "exportable", 'e', &ls_exportable_service },
        { FDB_OPTION_STRING, "host", 's', &host },
        { FDB_OPTION_STRING, "service_name", 'n', &service_name },
        { FDB_OPTION_STRING, "instance_id", 'i', &instance_id },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };

    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help)
    {
        std::cout << "FDBus - Fast Distributed Bus" << std::endl;
        std::cout << "    SDK version " << FDB_DEF_TO_STR(FDB_VERSION_MAJOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_MINOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_BUILD) << std::endl;
        std::cout << "    LIB version " << CFdbContext::getFdbLibVersion() << std::endl;
        std::cout << "Usage: lssvc[ -f][ -v]" << std::endl;
        std::cout << "List name of all services" << std::endl;
        std::cout << "    -f: keep monitoring service name" << std::endl;
        std::cout << "    -v: verbose mode" << std::endl;
        std::cout << "    -p: show public service" << std::endl;
        std::cout << "    -s host name: specify the host whose information will be retrieved" << std::endl;
        std::cout << "    -n service name: specify the name of service to monitor (only valid with -f option)" << std::endl;
        std::cout << "    -i instance id: specify the instance id to monitor; \"x\" suffixed means instance group (only valid with -f option)" << std::endl;
        return 0;
    }
    if (host)
    {
        ls_host_of_ns = host;
    }

    if (instance_id)
    {
        std::string inst_id = instance_id;
        if (inst_id.back() == FDB_ANY_INSTANCE_CHAR)
        {
            inst_id.pop_back();
            uint32_t group_id = FDB_EVENT_ALL_GROUPS;
            if (!inst_id.empty())
            {
                try
                {
                    group_id = std::stoi(inst_id);
                }
                catch (...)
                {
                    return -1;
                }
            }
            ls_instance_id = fdbMakeGroupEvent(group_id);
            std::cout << "Instance group ID: " << group_id << std::endl;
        }
        else
        {
            try
            {
                ls_instance_id = std::stoi(instance_id);
                std::cout << "Instance ID: " << ls_instance_id << std::endl;
            }
            catch (...)
            {
                std::cout << "Bad instance id: " << instance_id << std::endl;
                exit(-1);
            }
        }
    }

    if (service_name)
    {
        ls_service_name = service_name;
    }

    CFdbContext::enableLogger(false);
    
    CNameServerProxy nsp;
    nsp.connect();
    FDB_CONTEXT->start(FDB_WORKER_EXE_IN_PLACE);
    return 0;
}

