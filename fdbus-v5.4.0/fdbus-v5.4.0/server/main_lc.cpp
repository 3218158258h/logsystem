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

#include <set>
#include <string>
#include <fdbus/CFdbContext.h>
#include <fdbus/CBaseClient.h>
#include <fdbus/CFdbIfNameServer.h>
#include <iostream>
#include <stdlib.h>
#include <fdbus/fdb_option_parser.h>
#include <utils/Log.h>
#include <fdbus/CFdbSimpleMsgBuilder.h>

using namespace ipc::fdbus;

typedef std::vector<CBaseEndpoint *> FdbClientList_t;
FdbClientList_t lc_client_list;
static const char *fdb_endpoint_name = "org.fdbus.connection-fetcher";
static FdbSessionId_t fdb_query_sid = FDB_INVALID_ID;

class CConnectionFetcher : public CBaseClient
{
public:
    CConnectionFetcher()
        : CBaseClient(fdb_endpoint_name)
    {

    }
    ~CConnectionFetcher()
    {
        disconnect();
    }

protected:
    void onSidebandReply(CBaseJob::Ptr &msg_ref)
    {
        auto msg = castToMessage<CFdbMessage *>(msg_ref);
        if (msg->isStatus())
        {
            int32_t id;
            std::string reason;
            msg->decodeStatus(id, reason);
            LOG_I("CConnectionFetcher: status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());
            quit();
        }
        
        switch (msg->code())
        {
            case FDB_SIDEBAND_QUERY_PEER_SESSION_INFO:
            case FDB_SIDEBAND_QUERY_SESSION_INFO:
            {
                FdbMsgClientTable client_tbl;
                CFdbParcelableParser parser(client_tbl);
                if (!msg->deserialize(parser))
                {
                    fprintf(stderr, "CConnectionFetcher: unable to decode FdbMsgHostAddressList.\n");
                    quit();
                }
                for (auto it = lc_client_list.begin(); it != lc_client_list.end(); ++it)
                {
                    auto client = *it;
                    if ((client->nsName() == client_tbl.server_name()) &&
                            (client->instanceId() == client_tbl.instance_id()))
                    {
                        lc_client_list.erase(it);
                        printClients(client_tbl);
                        if (lc_client_list.empty())
                        {
                            quit();
                        }
                    }
                    break;
                }
            }
            break;
            default:
                CBaseEndpoint::onSidebandReply(msg_ref);
            break;
        }
    }

    void onOnline(const CFdbOnlineInfo &info)
    {
        if (fdbValidFdbId(fdb_query_sid))
        {
            FdbMsgQueryPeerInfo query;
            query.set_sid(fdb_query_sid);
            CFdbParcelableBuilder builder(query);
            invokeSideband(FDB_SIDEBAND_QUERY_PEER_SESSION_INFO, builder, 0, info.mQOS);
        }
        else
        {
            invokeSideband(FDB_SIDEBAND_QUERY_SESSION_INFO, 0, 0, 0, info.mQOS);
        }
    }

private:
    void quit()
    {
        exit(0);
    }

    void printClients(FdbMsgClientTable &client_tbl)
    {
        std::cout << "Server: " << client_tbl.server_name()
                  << ":" << client_tbl.instance_id()
                  << "; Endpoint: " << client_tbl.endpoint_name()
                  << "; Context Job Queue: ";
        if (client_tbl.has_context_job_queue_size())
        {
            std::cout << client_tbl.context_job_queue_size()
                      << "; Worker Job Queue: ";
        }
        else
        {
            std::cout << "NA"
                      << "; Worker Job Queue: ";
        }
        if (client_tbl.has_worker_job_queue_size())
        {
            std::cout << client_tbl.worker_job_queue_size();
        }
        else
        {
            std::cout << "NA";
        }
        std::cout << std::endl;

        auto &client_list = client_tbl.client_tbl();
        for (auto it = client_list.vpool().begin(); it != client_list.vpool().end(); ++it)
        {
            auto &client_info = *it;
            if (!client_info.peer_name().compare(fdb_endpoint_name))
            {
                continue;
            }
            const char *dog_status;
            if (client_info.dog_status() == FDB_DOG_ST_DIE)
            {
                dog_status = "DEAD";
            }
            else if (client_info.dog_status() == FDB_DOG_ST_OK)
            {
                dog_status = "OK";
            }
            else
            {
                dog_status = "NONE";
            }
            if (FDB_VALID_PORT(client_info.udp_port()))
            {
                std::cout << "    " << "" << client_info.peer_name()
                                    << " - address: " << client_info.peer_address()
                                    << "; pid: " << client_info.pid()
                                    << "; sid: " << client_info.sid()
                                    << "; security: " << client_info.security_level()
                                    << "; UDP: " << client_info.udp_port()
                                    << "; QOS: " << client_info.qos()
                                    << "; dog: " << dog_status
                                    << std::endl;
            }
            else
            {
                std::cout << "    " << "" << client_info.peer_name()
                                    << " - address: " << client_info.peer_address()
                                    << "; pid: " << client_info.pid()
                                    << "; sid: " << client_info.sid()
                                    << "; security: " << client_info.security_level()
                                    << "; QOS: " << client_info.qos()
                                    << "; dog: " << dog_status
                                    << std::endl;
            }
            printf("           |%-10s|%-10s|%-10s|%-10s|%-10s|%-10s|%-10s|%-10s|%-10s|%-10s|%-10s|%-10s\n",
                   "sync req",      "sync rep",     "async req",    "async rep",
                   "broadcast",     "publish",      "send",         "get evt",
                   "ret evt",     "sync stat",  "async stat", "Queue Size");
            printf("       tx: |%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d\n",
                    client_info.tcp_tx_statistics().mSyncRequest,
                    client_info.tcp_tx_statistics().mSyncReply,
                    client_info.tcp_tx_statistics().mAsyncRequest,
                    client_info.tcp_tx_statistics().mAsyncReply,
                    client_info.tcp_tx_statistics().mBroadcast,
                    client_info.tcp_tx_statistics().mPublish,
                    client_info.tcp_tx_statistics().mSend,
                    client_info.tcp_tx_statistics().mGetEvent,
                    client_info.tcp_tx_statistics().mReturnEvent,
                    client_info.tcp_tx_statistics().mSyncStatus,
                    client_info.tcp_tx_statistics().mAsyncStatus,
                    client_info.tx_queue_size()
                    );
            printf("       rx: |%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d|%-10d\n",
                    client_info.tcp_rx_statistics().mSyncRequest,
                    client_info.tcp_rx_statistics().mSyncReply,
                    client_info.tcp_rx_statistics().mAsyncRequest,
                    client_info.tcp_rx_statistics().mAsyncReply,
                    client_info.tcp_rx_statistics().mBroadcast,
                    client_info.tcp_rx_statistics().mPublish,
                    client_info.tcp_rx_statistics().mSend,
                    client_info.tcp_rx_statistics().mGetEvent,
                    client_info.tcp_rx_statistics().mReturnEvent,
                    client_info.tcp_rx_statistics().mSyncStatus,
                    client_info.tcp_rx_statistics().mAsyncStatus,
                    0
                    );
        }
    }
};

int main(int argc, char **argv)
{
    int32_t help = 0;
    int32_t sid = FDB_INVALID_ID;
	const struct fdb_option core_options[] = {
        { FDB_OPTION_INTEGER, "sid", 's', &sid },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };
    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help || (argc <= 1))
    {
        std::cout << "FDBus - Fast Distributed Bus" << std::endl;
        std::cout << "    SDK version " << FDB_DEF_TO_STR(FDB_VERSION_MAJOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_MINOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_BUILD) << std::endl;
        std::cout << "    LIB version " << CFdbContext::getFdbLibVersion() << std::endl;
        std::cout << "Usage: lsclt service_name[ service_name ...]" << std::endl;
        std::cout << "    List connected client of specified services" << std::endl;
        return 0;
    }
    fdb_query_sid = (FdbSessionId_t)sid;
    
    FDB_CONTEXT->enableLogger(false);
 
    for (int32_t i = 1; i < argc; ++i)
    {
        std::string server_addr;
        auto server_name = argv[i];
        server_addr = FDB_URL_SVC;
        server_addr += server_name;
        auto fetcher = new CConnectionFetcher();
        lc_client_list.push_back(fetcher);
        fetcher->connect(server_addr.c_str());
    }
    
    FDB_CONTEXT->start(FDB_WORKER_EXE_IN_PLACE);
    return 0;
}

