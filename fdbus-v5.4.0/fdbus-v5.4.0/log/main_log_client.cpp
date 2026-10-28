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

#include "fdb_log_config.h"
#include <fdbus/CFdbContext.h>
#include <fdbus/CBaseClient.h>
#include <fdbus/fdb_log_trace.h>
#include <fdbus/CLogProducer.h>
#include <utils/Log.h>
#include <stdlib.h>
#include <iostream>
#include "CLogPrinter.h"

using namespace ipc::fdbus;
class CLogClient : public CBaseClient
{
public:
    CLogClient()
        : CBaseClient(FDB_LOG_SERVER_NAME)
        , mLoggerCfgReplied(false)
        , mTraceCfgReplied(false)
    {
    }
protected:
    void onBroadcast(CBaseJob::Ptr &msg_ref)
    {
        CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
        switch (msg->code())
        {
            case NTF_FDBUS_LOG:
            {
                CFdbSimpleDeserializer deserializer(msg->getPayloadBuffer(), msg->getPayloadSize());
                mLogPrinter.outputFdbLog(deserializer, msg, std::cout, gFdbLogConfig.fdb_static_content);
            }
            break;
            case NTF_TRACE_LOG:
            {
                CFdbSimpleDeserializer deserializer(msg->getPayloadBuffer(), msg->getPayloadSize());
                mLogPrinter.outputTraceLog(deserializer, msg, std::cout, gFdbLogConfig.fdb_static_content);
            }
            break;
            default:
            break;
        }
    }
    
    void onOnline(const CFdbOnlineInfo &info)
    {
        if (gFdbLogConfig.fdb_sync)
        {
            invoke(REQ_LOG_SYNC);
        }
        if (gFdbLogConfig.fdb_info_mode)
        {
            invoke(REQ_GET_LOGGER_CONFIG);
            invoke(REQ_GET_TRACE_CONFIG);
        }
        else if (gFdbLogConfig.fdb_config_mode == CFG_MODE_ABSOLUTE)
        {
            FdbMsgLogConfig msg_cfg;
            fdb_fill_logger_config(msg_cfg, !gFdbLogConfig.fdb_disable_global_logger);
            {
            CFdbParcelableBuilder builder(msg_cfg);
            invoke(REQ_SET_LOGGER_CONFIG, builder);
            }

            FdbTraceConfig trace_cfg;
            fdb_fill_trace_config(trace_cfg, !gFdbLogConfig.fdb_disable_global_trace);
            {
            CFdbParcelableBuilder builder(trace_cfg);
            invoke(REQ_SET_TRACE_CONFIG, builder);
            }
        }
        else if (gFdbLogConfig.fdb_config_mode == CFG_MODE_INCREMENTAL)
        {
            invoke(REQ_GET_LOGGER_CONFIG);
            invoke(REQ_GET_TRACE_CONFIG);
        }
        else
        {
            //CFdbMsgSubscribeList subscribe_list;
            //addNotifyItem(subscribe_list, NTF_FDBUS_LOG);
            //addNotifyItem(subscribe_list, NTF_TRACE_LOG);
            //subscribe(subscribe_list);
            FdbLogStart log_start;
            log_start.set_cache_size(gFdbLogConfig.fdb_cache_size);
            CFdbParcelableBuilder builder(log_start);
            send(REQ_LOG_START, builder);
        }
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
                LOG_I("CLogClient: status is received: msg code: %d, id: %d, reason: %s\n", msg->code(), id, reason.c_str());
            }
            exit(0);
        }
        switch (msg->code())
        {
            case REQ_SET_LOGGER_CONFIG:
            case REQ_GET_LOGGER_CONFIG:
            {
                FdbMsgLogConfig in_config;
                CFdbParcelableParser parser(in_config);

                if (!msg->deserialize(parser))
                {
                    LOG_E("CLogServer: Unable to deserialize message!\n");
                    mLoggerCfgReplied = true;
                    Exit();
                    return;
                }

                if ((msg->code() == REQ_GET_LOGGER_CONFIG) &&
                    (gFdbLogConfig.fdb_config_mode == CFG_MODE_INCREMENTAL))
                {
                    if (fdb_update_logger_config(in_config, !gFdbLogConfig.fdb_disable_global_logger))
                    {
                        CFdbParcelableBuilder builder(in_config);
                        invoke(REQ_SET_LOGGER_CONFIG, builder);
                        break;
                    }
                }
                fdb_dump_logger_config(in_config);

                mLoggerCfgReplied = true;
                Exit();
            }
            break;
            case REQ_SET_TRACE_CONFIG:
            case REQ_GET_TRACE_CONFIG:
            {
                FdbTraceConfig in_config;
                CFdbParcelableParser parser(in_config);
                if (!msg->deserialize(parser))
                {
                    LOG_E("CLogServer: Unable to deserialize message!\n");
                    mLoggerCfgReplied = true;
                    Exit();
                    return;
                }
                if ((msg->code() == REQ_GET_TRACE_CONFIG) &&
                    (gFdbLogConfig.fdb_config_mode == CFG_MODE_INCREMENTAL))
                {
                    if (fdb_update_trace_config(in_config, !gFdbLogConfig.fdb_disable_global_logger))
                    {
                        CFdbParcelableBuilder builder(in_config);
                        invoke(REQ_SET_TRACE_CONFIG, builder);
                        break;
                    }
                }
                fdb_dump_trace_config(in_config);

                mTraceCfgReplied = true;
                Exit();
            }
            break;
            case REQ_LOG_SYNC:
                exit(0);
            break;
            default:
            break;
        }
    }
private:
    void Exit()
    {
        if (mLoggerCfgReplied && mTraceCfgReplied)
        {
            std::cout << "==== Configuration Items ====" << std::endl;
            fdb_print_configuration();
            exit(0);
        }
    }

    CLogPrinter mLogPrinter;
    bool mLoggerCfgReplied;
    bool mTraceCfgReplied;
};

int main(int argc, char **argv)
{
    if (!initLogConfig(argc, argv, true))
    {
        return 0;
    }

    CFdbContext::enableLogger(false);
    
    ::CLogClient log_client;
    log_client.enableReconnect(true);
    log_client.connect();
    FDB_CONTEXT->start(FDB_WORKER_EXE_IN_PLACE);
    return 0;
}
