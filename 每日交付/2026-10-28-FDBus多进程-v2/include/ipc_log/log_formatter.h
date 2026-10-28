#ifndef IPC_LOG_LOG_FORMATTER_H
#define IPC_LOG_LOG_FORMATTER_H

#include "ipc_log/log_record.h"

#include <string>

namespace ipc_log {
//汇聚线程输出前统一格式化，并转义正文控制字符以保持一条记录一行。
class LogFormatter
{
public:
    static std::string format(const LogRecord &record);
};
}

#endif
