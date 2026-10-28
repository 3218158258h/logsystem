# IPC 日志系统 · 设计文档（核心类图）


## 核心类图

```mermaid
classDiagram
    class LogLevel {
        <<enumeration>>
        Debug = 0
        Info = 1
        Warn = 2
        Error = 3
    }

    class LogRecord {
        +uint64_t timestamp_ms
        +LogLevel level
        +uint32_t pid
        +string module
        +string message
        +uint64_t sequence
        +uint64_t process_instance
    }

    class Logger {
        -shared_ptr~ILogTarget~ target_
        -atomic~uint32_t~ threshold_
        -uint64_t filtered_/truncated_/submitted_/rejected_
        +log(LogLevel, string, string) bool
        +debug()/info()/warn()/error()
        +setLevel(LogLevel)
        +stats() LoggerStats
    }

    class ILogTarget {
        <<interface>>
        +submit(LogRecord) bool
        +shutdown() ShutdownResult
        +stats() TargetStats
    }

    class FdbusLogTarget {
        -unique_ptr~Impl~ impl_
        +submit(LogRecord) bool
        +shutdown() ShutdownResult
        +processInstance() uint64_t
        +attachLogger(shared_ptr~Logger~) bool
    }

    class LogAggregator {
        -BoundedQueue~LogRecord~ queue_
        -shared_ptr~ILogSink~ console_sink_/file_sink_
        -ReorderBuffer reorder_
        +submit(LogRecord) bool
        +shutdown() ShutdownResult
        -run()/outputReady()/flushSinks()
    }

    class BoundedQueue~T~ {
        +tryPush(T) bool
        +waitPop(T&) bool
        +waitPopFor(T&, duration) bool
        +close()
        +closed() bool
    }

    class ReorderBuffer {
        -multiset~LogRecord~ records_
        -uint64_t max_seen_timestamp_
        +add(const LogRecord&)
        +releaseReady(now, force, vector&)
        +lateRecords() uint64_t
        +forcedReleases() uint64_t
    }

    class ILogSink {
        <<interface>>
        +write(LogLevel, string) SinkResult
        +flush() SinkResult
    }

    class ConsoleSink {
        +write(LogLevel, string) SinkResult
        +flush() SinkResult
    }

    class RotatingFileSink {
        -RotatingFileConfig config_
        -size_t current_bytes_
        +write(LogLevel, string) SinkResult
        +flush() SinkResult
        -rotate()
    }

    class LogServer {
        -unique_ptr~Impl~ impl_
        +start() bool
        +shutdown(milliseconds)
        +listWorkers(milliseconds) vector~WorkerInfo~
        +getWorkerLevel(uint64_t, milliseconds)
        +setWorkerLevel(uint64_t, LogLevel, milliseconds)
    }

    class LogFormatter {
        <<static>>
        +format(const LogRecord&) string
    }

    class WorkerInfo {
        +uint64_t session_id
        +uint64_t process_instance
        +uint32_t pid
        +LogLevel level
        +bool level_known
        +string status
    }

    FdbusLogTarget ..|> ILogTarget
    LogAggregator ..|> ILogTarget
    ConsoleSink ..|> ILogSink
    RotatingFileSink ..|> ILogSink

    Logger --> ILogTarget : target_
    Logger ..> LogRecord : 构造
    LogRecord --> LogLevel

    FdbusLogTarget *--> BoundedQueue~LogRecord~ : queue_
    FdbusLogTarget o--> Logger : logger_(weak)

    LogAggregator *--> BoundedQueue~LogRecord~ : queue_
    LogAggregator *--> ReorderBuffer : reorder_
    LogAggregator o--> ILogSink : console_sink_/file_sink_
    LogAggregator ..> LogFormatter : format()

    ReorderBuffer o--> LogRecord : records_
    LogServer o--> LogAggregator : aggregator_
    LogServer ..> WorkerInfo : 产生
```

## 图码对应

| 类 | 头文件 | 关键函数（代码锚点） |
|---|---|---|
| Logger | include/ipc_log/logger.h | `Logger::log` / `setLevel` |
| ILogTarget | include/ipc_log/log_target.h | `submit` / `shutdown` / `stats` |
| FdbusLogTarget | include/ipc_log/fdbus_transport.h | `submit` / `shutdown` / `attachLogger` |
| LogAggregator | include/ipc_log/log_aggregator.h | `submit` / `shutdown` / `run` |
| BoundedQueue | include/ipc_log/async_queue.h | `tryPush` / `waitPop` |
| ReorderBuffer | include/ipc_log/reorder_buffer.h | `add` / `releaseReady` |
| ILogSink | include/ipc_log/sink.h | `write` / `flush` |
| ConsoleSink | include/ipc_log/sink.h · src/console_sink.cpp | `write` |
| RotatingFileSink | include/ipc_log/sink.h · src/rotating_file_sink.cpp | `write` / `rotate` |
| LogServer | include/ipc_log/fdbus_transport.h | `start` / `listWorkers` / `setWorkerLevel` |
| LogFormatter | include/ipc_log/log_formatter.h | `format` |
| LogRecord / LogLevel | include/ipc_log/log_record.h | — |

> 注：答辩 HTML 内联的 SVG 版为清晰度省略了两条次要关系——`FdbusLogTarget → Logger`（`weak_ptr` 弱引用，用于控制回调改等级）、`ReorderBuffer → LogRecord`（内部 `std::multiset` 存储）；本页 Mermaid 版为完整版。
