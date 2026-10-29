#include "ipc_log/fdbus_transport.h"

#include <fdbus/CFdbContext.h>
#include <mutex>

namespace ipc_log {

//客户端和服务端共用进程内 FDBus context，统一管理启动与最后一次释放。
namespace detail {
namespace {
std::mutex &runtimeMutex() { static std::mutex value; return value; }
std::size_t &runtimeReferences() { static std::size_t value = 0; return value; }
bool &runtimeDestroyed() { static bool value = false; return value; }
}

bool acquireFdbusRuntime()
{
    std::lock_guard<std::mutex> lock(runtimeMutex());
    if (runtimeDestroyed()) return false;
    if (runtimeReferences() == 0)
    {
        ipc::fdbus::CFdbContext::enableLogger(false);
        if (!ipc::fdbus::CFdbContext::getInstance()->start()) return false;
    }
    ++runtimeReferences();
    return true;
}

void releaseFdbusRuntime()
{
    std::lock_guard<std::mutex> lock(runtimeMutex());
    if (runtimeReferences() == 0) return;
    if (--runtimeReferences() == 0)
    {
        runtimeDestroyed() = true;
        ipc::fdbus::CFdbContext::getInstance()->destroy();
    }
}
}

} //namespace ipc_log
