#include "ipc_log/log_formatter.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace ipc_log {

namespace {
//毫秒时间戳写成题目示例的可读形式 YYYY-MM-DD HH:MM:SS.mmm（本地时间）。
void appendTimestamp(std::ostringstream &output, uint64_t timestamp_ms)
{
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    const unsigned millis = static_cast<unsigned>(timestamp_ms % 1000);
    const std::tm *local = std::localtime(&seconds);
    if (!local)
    {
        output << timestamp_ms;
        return;
    }
    output << std::put_time(local, "%Y-%m-%d %H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0') << millis;
}

//正文中的控制字符写成可见转义，避免一条日志被换行或制表符拆成多行。
void appendEscaped(std::ostringstream &output, const std::string &value)
{
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it)
    {
        const unsigned char ch = static_cast<unsigned char>(*it);
        switch (ch)
        {
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            case '\\': output << "\\\\"; break;
            default:
                if (ch < 0x20 || ch == 0x7f)
                    output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                           << static_cast<unsigned int>(ch) << std::dec;
                else
                    output << static_cast<char>(ch);
        }
    }
}
}

std::string LogFormatter::format(const LogRecord &record)
{
    //格式固定为 [时间戳][等级][PID:xxx][模块][正文]，并保证一条输入只占一行。
    std::ostringstream output;
    output << '[';
    appendTimestamp(output, record.timestamp_ms);
    output << "][" << logLevelName(record.level) << "][PID:" << record.pid << "][";
    appendEscaped(output, record.module);
    output << "][";
    appendEscaped(output, record.message);
    output << "]\n";
    return output.str();
}

} //namespace ipc_log
