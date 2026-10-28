# IPC 日志系统整体架构基线 V1

> 建立：2026-10-26；按源码校正：2026-10-27。当前实现概览见 [根目录项目架构](../项目架构.md)，每日任务见 [交付计划第 6 节](implementation-plan.md#6-35-天交付与验收节奏)。
> 日期：2026-10-26。由当前 AI 按用户“先确定总架构”的委托制定，作为执行者的统一实现依据；不表示用户已逐项审阅，也不表示代码已实现或验证通过。
> 本文只固定开发所需的架构决策。正式架构图、类图、时序图及答辩设计文档在功能完善后制作。
> 后续修改模块边界、线程职责、通信模式或可靠性语义，先在本文记录原因和影响，再同步协作文档，避免实现过程中各自选择。

## 1. 已确定的技术与部署结构

- Linux、C++11、CMake；复用项目内 FDBus 5.4.0 源码，应用协议采用已安装的 Protobuf 3.21.12。
- 演示由一个 FDBus name_server、一个自研 log_server 和至少两个 log_worker 进程组成；name_server 是基础设施，不承担日志汇聚。
- 采用 FDBus client/server 请求与应答；服务名固定为 `svc://ipc_log_service`。
- worker 只生成并上报日志；主进程统一排序后输出到控制台和轮转文件。
- 复用同一套记录、过滤、队列、格式化与 Sink 实现；单进程及多进程代码在同一批次完成，随后统一编译联调。

| 决策 | 本项目选择及原因 | 其他方案的适用情况 |
|---|---|---|
| 汇聚拓扑 | 单一主进程集中输出，符合题目且文件所有权清楚 | 分布式收集用于规模更大的部署 |
| 通信模式 | client/server，便于明确接收确认和定向控制 | pub/sub 适合多个订阅者消费同一日志流 |
| 数据格式 | Protobuf，字段约定清楚，已有工具链 | 自定义字节协议减少依赖，但增加编解码与兼容工作 |
| 并发 | 有界队列与后台处理，FDBus 回调保持简短 | 同步业务线程直写会等待 I/O；无界队列无法控制内存 |
| 输出 | 控制台 + 按大小轮转文件 | 远程输出列入最终方案分析，不扩展核心实现 |

## 2. 模块与职责

| 模块 | 职责与边界 | 建议落点 |
|---|---|---|
| LogRecord / LogLevel | 统一记录字段和四级枚举；入队后记录不可变 | `include/ipc_log/log_record.h` |
| Logger | 进程独立的原子等级阈值；过滤、采集时间/PID、构造记录；调用目标的非阻塞入队接口 | `include/ipc_log/logger.h`、`src/logger.cpp` |
| ILogTarget | 统一 submit、shutdown、stats 契约，使 Logger 不直接依赖 FDBus 或文件实现 | `include/ipc_log/log_target.h` |
| BoundedQueue | 短临界区、多生产者、单消费者；容量限制、关闭唤醒；记录与控制屏障有序进入 | `include/ipc_log/async_queue.h` |
| LocalLogTarget | LogAggregator 的别名；单进程阶段复用同一队列、输出线程和 Sink | `include/ipc_log/local_log_target.h` |
| FdbusLogTarget | worker 使用：队列 + 一个后台发送线程；序列化、连接状态、在途请求与确认 | `src/fdbus_transport.cpp` |
| LogServer | FDBus CBaseServer 适配；校验消息、非阻塞入队、返回接收状态；不在回调中写文件 | `src/log_server.cpp` |
| LogAggregator | 主进程的 ILogTarget；本地与远端进入同一有界队列；一个消费线程负责重排、输出和退出排空 | `src/log_aggregator.cpp` |
| ReorderBuffer | 按时间戳和稳定键排序；时间窗口、空闲释放和容量上限 | `src/reorder_buffer.cpp` |
| LogFormatter / Sink | 最终格式化；ConsoleSink 与 RotatingFileSink；刷新及写入错误报告 | `src/log_formatter.cpp`、`console_sink.cpp`、`rotating_file_sink.cpp` |
| 应用入口 | 装配对象、信号退出、演示参数和进程启动顺序 | `apps/`、`scripts/run_demo.sh` |

Logger 与目标的最小接口契约：`submit(record)` 立即返回接受/丢弃结果；`shutdown()` 停止接收并发起排空，返回 `ShutdownResult`（是否排空、是否重复关闭及相关计数），最终 join 不保证固定退出时限；`stats()` 查询过滤、接受、丢弃与错误。不提供业务可调用的 flush。具体 C++ 声明在对应任务实现，不能改变上述语义。

## 3. 数据路径与线程分工

worker 路径：业务代码 → Logger 过滤与记录构造 → FdbusLogTarget 有界队列 → 发送线程序列化 → FDBus 异步请求 → 主进程。

主进程路径：FDBus 接收回调 → 校验并放入 LogAggregator 有界队列 → 汇聚线程 → ReorderBuffer → 最终文本格式化 → ConsoleSink + RotatingFileSink。

主进程本地 Logger 直接使用同一个 LogAggregator；不绕过排序和 Sink，不发 IPC 给自身。单进程演示将 Logger 绑定 LocalLogTarget。

| 执行上下文 | 允许执行 | 禁止执行 |
|---|---|---|
| 业务线程 | 等级检查、受限记录复制、短临界区入队 | 等待磁盘、网络、ACK 或队列腾出空间 |
| worker 发送线程 | 序列化、提交异步请求、等待确认/在线状态、退出时排空本地队列 | 无限提交请求或无限等待 |
| FDBus context 回调线程 | 在线/离线与应答状态更新；服务端解码校验、tryPush、立即应答 | 写文件、等待另一条回调或执行 shutdown |
| 主进程汇聚线程 | 队列消费、排序、两个 Sink 写入、退出时排空并刷新 | 回到业务调用路径执行 I/O |

每个涉及 IPC 的进程启动自己的 FDBus context 线程。本版不增加额外 FDBus callback worker；端点不绑定专用 CBaseWorker。线程归属与销毁顺序在通信联调时核对。

运行对象由应用入口统一持有：先停止生产并完成排空，再准备销毁端点，等待相关回调/线程结束，最后销毁队列、目标和 Sink；禁止异步回调引用已销毁对象。

## 4. 记录、格式与消息协议

LogRecord 字段固定为：`timestamp_ms`、`level`、`pid`、`module`、`message`、`sequence`、`process_instance`。实例标识区分 PID 重用，排序稳定键为 `(timestamp_ms, pid, process_instance, sequence)`；sequence 由目标在成功接受入队时按进程递增分配。

最终显示顺序固定：`[毫秒时间戳][等级][PID][模块][内容]`。采集时间使用系统时钟；等待、窗口截止及超时使用单调时钟。格式化和换行转义在最终输出线程执行。

Protobuf 放在 `proto/log_protocol.proto`，定义：

- `LOG_RECORD`：一条记录，作为日志请求的根消息。服务端检查字段、等级、长度与入队容量，显式回复 ACCEPTED 或拒绝原因；ACCEPTED 仅表示已入汇聚队列。
- 不定义业务可触发的 `FLUSH` 请求。退出时的排空由各进程内部 `shutdown` 完成，不经跨进程屏障。
- 等级控制：主进程向指定 session/实例发起查询或设置请求，worker 更新/读取 Logger 原子阈值后回复；控制命令为 list、get-level 和 set-level，目标使用 process_instance。
- 应答必须核对请求身份和消息类型；不能将 FDBus 自动应答当作应用层的接收或持久化确认。

每个 worker 的日志发送路径同时最多一个未确认请求；查询/设置等级使用独立的有界控制流程。发送线程使用异步 invoke，应答/超时回调只更新状态并唤醒发送线程，随后才提交下一条。这样应用队列与在途数据都受控；吞吐优化仅在实测需要时考虑批量，不先增加批处理协议。

必须限制 module/message/协议体长度、worker 队列容量、主进程队列容量与重排缓存容量；参数值在对应任务确定并暴露配置。只限制条数不足以控制超长消息带来的内存占用。

## 5. 排序、溢出与断连

- 等级过滤在生产端、记录入队前完成；每个进程独立设置等级。单次 log 调用不保证无锁或硬实时，但不等待 I/O/远端。
- worker 和主进程入队满时丢弃新日志，立即返回并累计相应计数；不会静默扩容或等待空间。
- 重排采用有序缓存（当前为 std::multiset）和有限窗口：按最大已见时间戳形成水位线；输入空闲达到条件时按序释放，容量受限或退出时提前输出。不将空闲释放条件表述为每条记录的严格延迟上限。
- 重排缓存达上限时提前输出最小记录，并统计强制释放次数。窗口外迟到日志保留、标记计数；正常退出也会提前排空。只保证有限窗口内汇聚排序，明确这些情况下可能跨窗口乱序。
- 文件按大小轮转，单一汇聚/输出线程持有文件；保留数量有界。写入、轮转或刷新失败进入统计及返回结果，不能报告成功。
- 未连接时发送线程等待在线状态，本地队列继续受容量约束；重新连通后继续发送尚未提交的记录。
- 对已经提交但超时/断连的请求不自动重发，记录“结果不确定”，避免重复落盘。worker 离线/请求等待按截止时间处理；主汇聚关闭仍等待输出线程 join，不能承诺阻塞 I/O 下总退出时间有界。
- 服务端崩溃/重启可能丢失此前仅确认接收的数据；重连不补承诺旧会话的持久化，不宣称零丢失、恰好一次或断电安全。

## 6. 退出排空与正常退出

题目要求“退出时 flush”，实现为日志系统内部的退出排空，不提供业务代码可调用的 flush 接口，也不设跨进程 flush 屏障。这样业务侧不能影响日志系统，收尾路径也更短。

worker：`shutdown` 停止接收 → 关闭本地队列 → 发送线程取完关闭前积压并逐条上报 → 等待发送线程结束（受 `shutdown_timeout` 限制）→ 超时则停止发送并把未发出记录计为 `dropped` → 关闭端点与线程。

主进程：`LogAggregator::shutdown` 停止接收 → 关闭队列 → 消费线程取完关闭前积压 → 强制释放重排窗口剩余记录 → 刷新两个 Sink → join。`LogServer::shutdown` 只负责停止接收、清理控制请求和销毁端点，不代替聚合器排空。

`ShutdownResult` 汇总是否排空、是否重复关闭、接受/丢弃/拒收/结果不确定/写错误等计数；应用入口据此决定退出码。刷新输出流不等于执行 fsync 或保证断电恢复。

演示退出顺序固定：worker 停止生产并排空本地队列 → 主进程排空并停止 → name_server 停止。信号处理函数只设置退出标志，由正常线程执行关闭流程。退出排空只覆盖已经进入对应队列的记录；SIGKILL/崩溃、仍在传输层的记录可能丢失。

## 7. CMake 与实现顺序

- `ipc_log_core`：记录、Logger、队列、格式化、本地目标、排序/汇聚、Sink；依赖 Threads，不依赖 FDBus。
- `ipc_log_transport`：协议生成、FDBus 客户端/服务端适配；依赖 core、Protobuf 和项目内 FDBus 库。
- `log_server`、`log_worker`：应用入口，链接上述模块。
- `local_log_demo`：只链接 core，用于单进程阶段验证；最小 FDBus 连通检查直接演进为最终应用，不另建一套正式产品。
- 继续保留现有 ExternalProject 的 FDBus 独立构建；主工程补齐导入库、头文件、目标依赖及 Linux 运行时库路径。现有 `ipc_log_system` 继续作为总构建入口。
- 执行者在完整实现批次中补齐所有真实库/应用目标；已有 FDBus 编译兼容设置保留，不以核心占位实现冒充完成。

功能与集中调级已有实现，当前按 `implementation-plan.md` 第 6 节的 3.5 天安排交付。先完成核心阶段独立复验，再做 FDBus 多进程/控制专项验收，最后制作正式图文和答辩材料；不沿用已过期的 T01–T09 执行顺序。

## 8. 范围与变更规则

首版交付范围：四级日志、进程独立动态等级、规定格式、有界异步处理、丢弃/错误计数、两个 Sink 与轮转、FDBus 多 worker 汇聚、有限窗口排序、断连处理和正常退出排空、可运行演示。

全局配置中心、跨机远程收集实现、磁盘重放队列、自动重传去重、复杂插件系统和 Web 界面不进入本版核心实现。协议批处理只在实测确有性能瓶颈时另行评估。

模块拆分、线程职责、通信确认与退出排空语义属于架构约定；具体函数签名、队列容量、窗口和切割阈值由执行者在对应任务确定。后者可以调优，不能借调优改变前者。

### 制定依据与当前验证边界

- 已核对题目、项目内 FDBus 5.4.0 公共头文件及 client-server 示例。用户反馈 FDBus 编译问题已解决；当前完整源码 Linux 回归和核心阶段包独立构建仍待验证，历史运行结果不代替当前验收。
- `public/fdbus/CFdbBaseObject.h` 的异步 invoke、超时与 onReply 接口支持请求/应答设计。
- `public/fdbus/CFdbMessage.h` 的 reply 接口，以及 `fdbus/CFdbBaseObject.cpp::callInvoke`、`fdbus/CFdbMessage.cpp::autoReply` 表明自动回复与请求引用生命周期有关，通信实现必须核对并测试延后应答。
- 纠正旧草图：“invoke(code, builder) 是异步无应答”不准确；异步请求可以收到 onReply 应答。这里区分异步调用、单向 send、应用接收确认和输出确认。
