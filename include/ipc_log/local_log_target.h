#ifndef IPC_LOG_LOCAL_LOG_TARGET_H
#define IPC_LOG_LOCAL_LOG_TARGET_H

#include "ipc_log/log_aggregator.h"

namespace ipc_log {

//本地日志直接复用汇聚器，避免额外的转发对象。
typedef LogAggregator LocalLogTarget;

} //namespace ipc_log

#endif
