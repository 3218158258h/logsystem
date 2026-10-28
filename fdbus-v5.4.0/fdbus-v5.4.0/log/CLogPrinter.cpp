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

#include "CLogPrinter.h"
#include <fdbus/CNanoTimer.h>
#include <fdbus/CBaseSysDep.h>
#include <fdbus/CFdbSimpleMsgBuilder.h>
#include <utils/Log.h>
#include <stdlib.h>
#include <string>

namespace ipc {
namespace fdbus {
CLogPrinter::CLogPrinter()
{
}

void CLogPrinter::outputFdbLog(LogInfo &log_info, std::ostream &output)
{
    if (log_info.mOptions & FDB_LOG_STATIC_CONTENT)
    {
        output << "F+" << 0 << "@" << log_info.mHostName << "+"
               << log_info.mSender << "->" << log_info.mReceiver << "+"
               << log_info.mBusName << "+"
               << log_info.mObjId << "+"
               << CFdbMessage::getMsgTypeName((EFdbMessageType)log_info.mMsgType) << "+"
               << log_info.mMsgCode << "+"
               << log_info.mTopic << "+"
               << 0 << "+"
               << log_info.mPayloadSize << "+Q"
               << (unsigned short)log_info.mQOS << "+"
               << 0 << "+ {"
               << std::endl;
    }
    else
    {
        output << "F+" << log_info.mPid << "@" << log_info.mHostName << "+"
               << log_info.mSender << "->" << log_info.mReceiver << "+"
               << log_info.mBusName << "+"
               << log_info.mObjId << "+"
               << CFdbMessage::getMsgTypeName((EFdbMessageType)log_info.mMsgType) << "+"
               << log_info.mMsgCode << "+"
               << log_info.mTopic << "+"
               << (fdbValidFdbId(log_info.mMsgSn) ? log_info.mMsgSn : 0) << "+"
               << log_info.mPayloadSize << "+Q"
               << (unsigned short)log_info.mQOS << "+"
               << log_info.mTimeStamp << "+ {"
               << std::endl;
    }

    if (log_info.mOptions & FDB_LOG_IS_STRING)
    {
        if (log_info.mData)
        {
            output << (const char *)log_info.mData << "}" << std::endl;
        }
    }
    else
    {
        output << "No log! Data size: " << log_info.mDataSize << std::endl << "}" << std::endl;
    }
    if (log_info.mOptions & FDB_LOG_APPEND_LINE_END)
    {
        output << std::endl;
    }
}

void CLogPrinter::outputFdbLog(CFdbSimpleDeserializer &deserializer,
                               CFdbMessage *log_msg,
                               std::ostream &output,
                               bool force_static_content)
{
    LogInfo info;
    deserializer >> info.mPid
                 >> info.mHostName
                 >> info.mSender
                 >> info.mReceiver
                 >> info.mBusName
                 >> info.mMsgType
                 >> info.mMsgCode
                 >> info.mTopic
                 >> info.mTimeStamp
                 >> info.mPayloadSize
                 >> info.mMsgSn
                 >> info.mObjId
                 >> info.mQOS
                 >> info.mOptions;
    if (info.mOptions & FDB_LOG_IS_STRING)
    {
        fdb_string_len_t str_len;
        deserializer >> str_len;
        info.mData = str_len ? (void *)deserializer.pos() : 0;
        info.mDataSize = str_len;
    }
    else
    {
        deserializer >> info.mDataSize;
        info.mData = 0;
    }

    if (force_static_content)
    {
        info.mOptions |= FDB_LOG_STATIC_CONTENT;
    }

    outputFdbLog(info, output);

}

void CLogPrinter::outputTraceLog(TraceInfo &trace_info, std::ostream &output)
{
    static const char *level_name[] = {
        "D+V+",
        "D+D+",
        "D+I+",
        "D+W+",
        "D+E+",
        "D+F+",
        "D+S+"
    };

    if (trace_info.mOptions & FDB_LOG_STATIC_CONTENT)
    {
        output << level_name[trace_info.mLogLevel]
               << trace_info.mTag << "+"
               << 0 << "@" << trace_info.mHostName << "+"
               << 0 << "+ "
               << trace_info.mData;
    }
    else
    {
        output << level_name[trace_info.mLogLevel]
               << trace_info.mTag << "+"
               << trace_info.mPid << "@" << trace_info.mHostName << "+"
               << trace_info.mTimeStamp << "+ "
               << trace_info.mData;
    }
    if (trace_info.mOptions & FDB_LOG_APPEND_LINE_END)
    {
        output << std::endl;
    }
}

void CLogPrinter::outputTraceLog(CFdbSimpleDeserializer &deserializer,
                                 CFdbMessage *trace_msg,
                                 std::ostream &output,
                                 bool force_static_content)
{
    TraceInfo info;
    deserializer >> info.mPid
                 >> info.mTag
                 >> info.mHostName
                 >> info.mTimeStamp
                 >> info.mLogLevel
                 >> info.mOptions;
    if (info.mLogLevel >= FDB_LL_MAX)
    {
        return;
    }
    if (force_static_content)
    {
        info.mOptions |= FDB_LOG_STATIC_CONTENT;
    }

    fdb_string_len_t str_len = 0;
    deserializer >> str_len;
    info.mData = str_len ? (const char *)deserializer.pos() : "\n";
    outputTraceLog(info, output);
}
                                 }
                                 }
                                 
