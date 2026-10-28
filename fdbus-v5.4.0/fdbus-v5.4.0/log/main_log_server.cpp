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
#include <fdbus/CBaseServer.h>
#include <fdbus/fdb_option_parser.h>
#include <fdbus/fdb_log_trace.h>
#include <fdbus/CLogProducer.h>
#include <utils/Log.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <iostream>
#include "CLogPrinter.h"
#include "CFdbLogCache.h"
#include "CLogFileManager.h"

namespace ipc {
namespace fdbus {
class CLogServer : public CBaseServer
{
public:
    CLogServer()
        : CBaseServer(FDB_LOG_SERVER_NAME)
        , mLogCache(gFdbLogConfig.fdb_cache_size * 1024)
        , mFileManager(gFdbLogConfig.fdb_log_path.c_str(), 0,
                       gFdbLogConfig.fdb_max_log_storage_size * 1024,
                       gFdbLogConfig.fdb_max_log_file_size * 1024,
                       gFdbLogConfig.fdb_log_compress)
    {
        if (gFdbLogConfig.fdb_exportable_level < 0)
        {
            setExportableLevel(FDB_EXPORTABLE_DOMAIN);
        }
        else
        {
            setExportableLevel(gFdbLogConfig.fdb_exportable_level);
        }
        mEnableGlogalLogger = checkLogEnabled(gFdbLogConfig.fdb_disable_global_logger, false);
        mEnableGlobalTrace = checkLogEnabled(gFdbLogConfig.fdb_disable_global_trace, false);
    }
protected:
    void onInvoke(CBaseJob::Ptr &msg_ref)
    {
        auto msg = castToMessage<CFdbMessage *>(msg_ref);
        switch (msg->code())
        {
            case REQ_FDBUS_LOG:
            {
                if (!gFdbLogConfig.fdb_disable_std_output || mFileManager.logEnabled())
                {
                    CFdbSimpleDeserializer deserializer(msg->getPayloadBuffer(), msg->getPayloadSize());
                    std::ostringstream ostream;
                    mLogPrinter.outputFdbLog(deserializer, msg, ostream, gFdbLogConfig.fdb_static_content);

                    if (!gFdbLogConfig.fdb_disable_std_output)
                    {
                        std::cout << ostream.str();
                    }
                    if (mFileManager.logEnabled())
                    {
                        mFileManager.store(ostream.str());
                    }
                }
                forwardLogData(NTF_FDBUS_LOG, msg);
            }
            break;
            case REQ_SET_LOGGER_CONFIG:
            {
                FdbMsgLogConfig in_config;
                CFdbParcelableParser parser(in_config);
                if (!msg->deserialize(parser))
                {
                    LOG_E("CLogServer: Unable to deserialize message!\n");
                    return;
                }
                fdb_dump_logger_config(in_config);
                mEnableGlogalLogger = checkLogEnabled(gFdbLogConfig.fdb_disable_global_logger,
                                                            mLoggerClientTbl.empty());

                FdbMsgLogConfig out_config;
                fdb_fill_logger_config(out_config, mEnableGlogalLogger);
                CFdbParcelableBuilder builder(out_config);
                broadcast(NTF_LOGGER_CONFIG, builder);
                msg->reply(msg_ref, builder);
            }
            break;
            case REQ_GET_LOGGER_CONFIG:
            {
                FdbMsgLogConfig out_config;
                fdb_fill_logger_config(out_config, mEnableGlogalLogger);
                CFdbParcelableBuilder builder(out_config);
                msg->reply(msg_ref, builder);
            }
            break;
            case REQ_TRACE_LOG:
            {
                if (!gFdbLogConfig.fdb_disable_std_output || mFileManager.logEnabled())
                {
                    CFdbSimpleDeserializer deserializer(msg->getPayloadBuffer(), msg->getPayloadSize());
                    std::ostringstream ostream;
                    mLogPrinter.outputTraceLog(deserializer, msg, ostream, gFdbLogConfig.fdb_static_content);

                    if (!gFdbLogConfig.fdb_disable_std_output)
                    {
                        std::cout << ostream.str();
                    }
                    if (mFileManager.logEnabled())
                    {
                        mFileManager.store(ostream.str());
                    }
                }
                forwardLogData(NTF_TRACE_LOG, msg);
            }
            break;
            case REQ_SET_TRACE_CONFIG:
            {
                FdbTraceConfig in_config;
                CFdbParcelableParser parser(in_config);
                if (!msg->deserialize(parser))
                {
                    LOG_E("CLogServer: Unable to deserialize message!\n");
                    return;
                }
                fdb_dump_trace_config(in_config);

                mLogCache.resize(gFdbLogConfig.fdb_cache_size * 1024);
                mFileManager.setWorkingPath(gFdbLogConfig.fdb_log_path.c_str());
                mFileManager.setStorageSize(gFdbLogConfig.fdb_max_log_storage_size * 1024,
                                            gFdbLogConfig.fdb_max_log_file_size * 1024);

                mEnableGlobalTrace = checkLogEnabled(gFdbLogConfig.fdb_disable_global_trace,
                                                            mTraceClientTbl.empty());

                FdbTraceConfig out_config;
                fdb_fill_trace_config(out_config, mEnableGlobalTrace);
                CFdbParcelableBuilder builder(out_config);
                broadcast(NTF_TRACE_CONFIG, builder);
                msg->reply(msg_ref, builder);
            }
            break;
            case REQ_GET_TRACE_CONFIG:
            {
                FdbTraceConfig out_config;
                fdb_fill_trace_config(out_config, mEnableGlobalTrace);
                CFdbParcelableBuilder builder(out_config);
                msg->reply(msg_ref, builder);
            }
            break;
            case REQ_LOG_START:
            {
                FdbLogStart log_start;
                CFdbParcelableParser parser(log_start);
                if (!msg->deserialize(parser))
                {
                    LOG_E("CLogServer: Unable to deserialize message!\n");
                    return;
                }
                auto session = msg->getSession();

                subscribe(session, NTF_FDBUS_LOG, objId(), 0, 0);
                onSubscribeFDBusLogger(msg->session());

                subscribe(session, NTF_TRACE_LOG, objId(), 0, 0);
                onSubscribeTraceLogger(msg->session());

                auto cache_size = log_start.cache_size();
                if (cache_size > 0)
                {
                    cache_size *= 1024;
                }
                mLogCache.dump(this, session, cache_size);
            }
            case REQ_LOG_SYNC:
            {
                mFileManager.sync();
                msg->reply(msg_ref);
            }
            break;
            default:
            break;
        }
    }
    
    void onSubscribe(CBaseJob::Ptr &msg_ref)
    {
        auto msg = castToMessage<CFdbMessage *>(msg_ref);
        const CFdbMsgSubscribeItem *sub_item;
        FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
        {
            switch (sub_item->msg_code())
            {
                case NTF_LOGGER_CONFIG:
                {
                    FdbMsgLogConfig cfg;
                    fdb_fill_logger_config(cfg, mEnableGlogalLogger);
                    CFdbParcelableBuilder builder(cfg);
                    msg->broadcast(NTF_LOGGER_CONFIG, builder);
                }
                break;
                case NTF_TRACE_CONFIG:
                {
                    FdbTraceConfig cfg;
                    fdb_fill_trace_config(cfg, mEnableGlobalTrace);
                    CFdbParcelableBuilder builder(cfg);
                    msg->broadcast(NTF_TRACE_CONFIG, builder);
                }
                break;
                case NTF_FDBUS_LOG:
                {
                    onSubscribeFDBusLogger(msg->session());
                }
                break;
                case NTF_TRACE_LOG:
                {
                    onSubscribeTraceLogger(msg->session());
                }
                break;
            }
        }
        FDB_END_FOREACH_SIGNAL()
    }

    void onOnline(const CFdbOnlineInfo &info)
    {
    }

    void onOffline(const CFdbOnlineInfo &info)
    {
        {
        auto it = mLoggerClientTbl.find(info.mSid);
        if (it != mLoggerClientTbl.end())
        {
            delete it->second;
            mLoggerClientTbl.erase(it);
            if (mLoggerClientTbl.empty())
            {
                mEnableGlogalLogger = checkLogEnabled(gFdbLogConfig.fdb_disable_global_logger, true);
                FdbMsgLogConfig cfg;
                fdb_fill_logger_config(cfg, mEnableGlogalLogger);
                CFdbParcelableBuilder builder(cfg);
                broadcast(NTF_LOGGER_CONFIG, builder);
            }
        }
        }
        {
        auto it = mTraceClientTbl.find(info.mSid);
        if (it != mTraceClientTbl.end())
        {
            delete it->second;
            mTraceClientTbl.erase(it);
            if (mTraceClientTbl.empty())
            {
                mEnableGlobalTrace = checkLogEnabled(gFdbLogConfig.fdb_disable_global_trace, true);
                FdbTraceConfig cfg;
                fdb_fill_trace_config(cfg, mEnableGlobalTrace);
                CFdbParcelableBuilder builder(cfg);
                broadcast(NTF_TRACE_CONFIG, builder);
            }
        }
        }
    }
private:
    struct CLogClientCfg
    {
        CLogClientCfg( )
            : mUnused(false)
        {}
        bool mUnused;
    };
    typedef std::map<FdbSessionId_t, CLogClientCfg *> LoggerClientTbl_t;
    
    LoggerClientTbl_t mLoggerClientTbl;

    struct CTraceClientCfg
    {
        CTraceClientCfg( )
            : mUnused(false)
        {}
        bool mUnused;
    };
    typedef std::map<FdbSessionId_t, CTraceClientCfg *> TraceClientTbl_t;
    
    TraceClientTbl_t mTraceClientTbl;

    CLogPrinter mLogPrinter;
    CFdbLogCache mLogCache;
    CLogFileManager mFileManager;
    bool mEnableGlogalLogger;
    bool mEnableGlobalTrace;

    bool checkLogEnabled(bool global_disable, bool no_client_connected)
    {
        if (global_disable)
        {
            return false; // if log is disabled globally, don't generate log message
        }
        else if (mLogCache.size() ||            // log cache is enabled
                 !gFdbLogConfig.fdb_disable_std_output ||     // showing at stdout
                 !no_client_connected ||        // any log viewer is connected
                 mFileManager.logEnabled())     // logging to file is enabled
        {
            return true;
        }
        else
        {
            return false; // don't generat any log message
        }
    }

    void onSubscribeFDBusLogger(FdbSessionId_t sid)
    {
        bool is_first = mLoggerClientTbl.empty();
        auto it = mLoggerClientTbl.find(sid);
        if (it == mLoggerClientTbl.end())
        {
            mLoggerClientTbl[sid] = new CLogClientCfg();
            mEnableGlogalLogger = checkLogEnabled(gFdbLogConfig.fdb_disable_global_logger, false);
        }
        if (is_first)
        {
            FdbMsgLogConfig cfg;
            fdb_fill_logger_config(cfg, mEnableGlogalLogger);
            CFdbParcelableBuilder builder(cfg);
            broadcast(NTF_LOGGER_CONFIG, builder);
        }
    }

    void onSubscribeTraceLogger(FdbSessionId_t sid)
    {
        bool is_first = mTraceClientTbl.empty();
        auto it = mTraceClientTbl.find(sid);
        if (it == mTraceClientTbl.end())
        {
            mTraceClientTbl[sid] = new CTraceClientCfg();
            mEnableGlobalTrace = checkLogEnabled(gFdbLogConfig.fdb_disable_global_trace, false);
        }
        if (is_first)
        {
            FdbTraceConfig cfg;
            fdb_fill_trace_config(cfg, mEnableGlobalTrace);
            CFdbParcelableBuilder builder(cfg);
            broadcast(NTF_TRACE_CONFIG, builder);
        }
    }

    void forwardLogData(FdbEventCode_t code, CFdbMessage *msg)
    {
        auto payload = (uint8_t *)msg->getPayloadBuffer();
        int32_t size = msg->getPayloadSize();
        mLogCache.push(payload, size, code);

        if (((code == NTF_FDBUS_LOG) && !mLoggerClientTbl.empty()) ||
            ((code == NTF_TRACE_LOG) && !mTraceClientTbl.empty()))
        {
            broadcastLogNoQueue(code, payload, size, 0);
        }
    }
};
}
}
using namespace ipc::fdbus;

int main(int argc, char **argv)
{
    if (!initLogConfig(argc, argv, false))
    {
        return 0;
    }

    CFdbContext::enableLogger(false);
    
    CLogServer log_server;
    log_server.bind();
    FDB_CONTEXT->start(FDB_WORKER_EXE_IN_PLACE);
    return 0;
}

