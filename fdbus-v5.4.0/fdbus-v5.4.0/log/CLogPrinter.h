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

#ifndef __CLOGPRINTER_H__
#define __CLOGPRINTER_H__
#include <string>
#include <iostream>
#include <fdbus/CFdbMessage.h>

namespace ipc {
namespace fdbus {
#define FDB_LOG_APPEND_LINE_END (1 << 0)
#define FDB_LOG_IS_STRING       (1 << 1)
#define FDB_LOG_STATIC_CONTENT  (1 << 2)

class CFdbSimpleDeserializer;
class CLogPrinter
{
public:
    struct LogInfo
    {
        uint32_t mPid;
        std::string mHostName;
        std::string mSender;
        std::string mReceiver;
        std::string mBusName;
        std::string mTopic;
        uint8_t mMsgType;
        FdbMsgCode_t mMsgCode;
        std::string mTimeStamp;
        int32_t mPayloadSize;
        FdbMsgSn_t mMsgSn;
        FdbObjectId_t mObjId;
        int8_t mQOS;
        const void *mData;
        int32_t mDataSize;
        uint8_t mOptions;
    };
    struct TraceInfo
    {
        uint32_t mPid;
        std::string mTag;
        std::string mHostName;
        std::string mTimeStamp;
        uint8_t mLogLevel;
        uint8_t mOptions;
        const char *mData;
    };
    CLogPrinter();
    void outputFdbLog(CFdbSimpleDeserializer &log_info,
                      CFdbMessage *log_msg,
                      std::ostream &output,
                      bool force_static_content);
    static void outputFdbLog(LogInfo &log_info, std::ostream &output);
    void outputTraceLog(CFdbSimpleDeserializer &trace_info,
                        CFdbMessage *trace_msg,
                        std::ostream &output,
                        bool force_static_content);
    static void outputTraceLog(TraceInfo &trace_info, std::ostream &output);
};
}
}
#endif
