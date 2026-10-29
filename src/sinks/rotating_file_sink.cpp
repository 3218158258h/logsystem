#include "ipc_log/sink.h"

#include <cstdio>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

namespace ipc_log {

namespace {
//逐级创建父目录，并区分“不存在”与权限/路径类型等真实文件系统错误。
SinkResult ensureParentDirectories(const std::string &path)
{
    const std::string::size_type last_separator = path.find_last_of("/\\");//找到最后一个分隔符
    if (last_separator == std::string::npos) return SinkResult();
    const std::string parent = path.substr(0, last_separator);//获取上一级目录
    if (parent.empty()) return SinkResult();//为空则在本目录创建文件

    for (std::string::size_type i = 1; i <= parent.size(); ++i)//不为空
    {
        if (i != parent.size() && parent[i] != '/' && parent[i] != '\\') continue;//跳过分隔符
        const std::string directory = parent.substr(0, i);
        if (directory.empty() || (directory.size() == 2 && directory[1] == ':')) continue;
        struct stat info;
        if (stat(directory.c_str(), &info) == 0)
        {
            if ((info.st_mode & S_IFDIR) == 0)//已存在且不是同名文件
                return SinkResult(false, "log path component is not a directory: " + directory);
            continue;
        }
        if (errno != ENOENT)//其他错误返回
            return SinkResult(false, "cannot inspect log directory: " + directory);
        //创建目录
#ifdef _WIN32
        if (_mkdir(directory.c_str()) != 0 && errno != EEXIST)
#else
        if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST)
#endif
            return SinkResult(false, "cannot create log directory: " + directory);
    }
    return SinkResult();
}
}

//确保上级目录和配置正常，然后打开/创建日志文件
RotatingFileSink::RotatingFileSink(const RotatingFileConfig &config)
    : config_(config), current_bytes_(0)
{
    //从现有文件大小继续计数，避免进程重启后首批写入绕过切割阈值。
    if (config_.path.empty() || config_.max_bytes == 0)
        throw std::invalid_argument("file path and max_bytes must be nonzero");
    if (config_.backup_count > 100)
        throw std::invalid_argument("backup_count must not exceed 100");
    const SinkResult directories = ensureParentDirectories(config_.path);//确保路径不出错
    if (!directories.ok) throw std::runtime_error(directories.message);
    //读取已有日志文件的大小
    std::ifstream existing(config_.path.c_str(), std::ios::binary | std::ios::ate);
    if (existing) current_bytes_ = static_cast<std::size_t>(existing.tellg());
    SinkResult result = openFile();//创建或打开日志文件
    if (!result.ok) throw std::runtime_error(result.message);
}

RotatingFileSink::~RotatingFileSink()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) stream_.close();
}

std::string RotatingFileSink::backupPath(std::size_t index) const
{
    std::ostringstream path;
    path << config_.path << '.' << index;
    return path.str();
}

SinkResult RotatingFileSink::openFile()
{
    stream_.open(config_.path.c_str(), std::ios::out | std::ios::app | std::ios::binary);
    if (!stream_) return SinkResult(false, "cannot open log file: " + config_.path);
    return SinkResult();
}

SinkResult RotatingFileSink::rotate()
{
    //先刷新并关闭当前文件，再倒序移动备份；失败时向调用方报告，后续写入可重开主文件。
    stream_.flush();
    if (!stream_) return SinkResult(false, "failed to flush log file before rotation");
    stream_.close();

    if (config_.backup_count == 0)
    {
        if (std::remove(config_.path.c_str()) != 0)
            return SinkResult(false, "cannot remove full log file during rotation");
    }
    else
    {
        const std::string oldest = backupPath(config_.backup_count);
        errno = 0;
        if (std::remove(oldest.c_str()) != 0 && errno != ENOENT)
            return SinkResult(false, "cannot remove oldest log backup: " + oldest);
        //按备份编号从大到小后移，先腾出目标文件，避免覆盖尚未移动的备份。
        for (std::size_t i = config_.backup_count; i > 1; --i)
        {
            const std::string from = backupPath(i - 1);
            const std::string to = backupPath(i);
            if (std::rename(from.c_str(), to.c_str()) != 0 && errno != ENOENT)
                return SinkResult(false, "cannot rotate backup file: " + from);
        }
        if (std::rename(config_.path.c_str(), backupPath(1).c_str()) != 0)
            return SinkResult(false, "cannot move full log file during rotation");
    }
    current_bytes_ = 0;
    return openFile();
}

SinkResult RotatingFileSink::write(LogLevel, const std::string &line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open())
    {
        SinkResult opened = openFile();
        if (!opened.ok) return opened;
    }
    const std::size_t remaining_bytes = current_bytes_ < config_.max_bytes ?
                                        config_.max_bytes - current_bytes_ : 0;
    //空文件允许先写入一条超阈值的大记录，之后再切割，避免空文件反复轮转。
    if (current_bytes_ > 0 && line.size() > remaining_bytes)
    {
        SinkResult rotated = rotate();
        if (!rotated.ok) return rotated;
    }
    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!stream_) return SinkResult(false, "log file write failed: " + config_.path);
    current_bytes_ += line.size();
    return SinkResult();
}

SinkResult RotatingFileSink::flush()
{
    //flush 只请求 C++ stream 刷新到操作系统，不承诺 fsync 或掉电持久化。
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) stream_.flush();
    if (!stream_) return SinkResult(false, "log file flush failed: " + config_.path);
    return SinkResult();
}

} //namespace ipc_log
