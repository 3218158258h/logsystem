# 沟通问题与 AI 纠偏记录

> 更新日期：2026-10-27。只记录对话、源码、构建日志或用户运行输出支持的事实；历史事件日期未能确认时明确标注，不反推日期。

## 状态说明

- **已观察**：现象有用户消息、源文件或命令输出作为依据。
- **已修改**：对应代码/流程已经调整，但修改后的同一版本尚无回归证据。
- **已验证**：对应版本有构建或运行结果；注明是用户报告还是本地检查。
- **待核实**：现有证据不足以确认根因或具体修改差异。
- 记录分类区分 AI 实现缺陷、需求覆盖不足、需求理解偏差、环境/目录问题和协作过程问题，不把所有故障统归于 AI 代码。

## R3-01 — Linux 项目副本曾缺少根 CMakeLists.txt

- 日期：记录日期 2026-10-27；事件日期未单独核实。
- 分类：环境与目录 / 协作过程。
- 现象与用户反馈：用户在 Linux 项目目录执行 `cmake -S . -B build ...`，CMake 报告 `/home/test/workspace/HJM-fast` 不含 `CMakeLists.txt`；当时 `ls` 输出也未列出该文件。后续用户展示的同目录清单已有根 `CMakeLists.txt`，再次配置成功并生成 `build`。
- 证据：用户对话中的 `ls`、CMake 报错和随后配置成功输出；当前 Windows 工作树根目录存在 `CMakeLists.txt`。
- 原建议或实现：早期交接按 Linux 副本当时内容执行；现有证据不能确认 CMakeLists 是何时、如何传入 Linux 副本。
- 根因：项目副本同步/工作目录内容差异可观察；具体漏拷或目录切换动作待核实。
- 处理方式：当前代码以 HJM-fast 根目录为唯一开发源；每日快照将显式包含根 CMakeLists 和运行所需文件。
- 验证结果：已观察；用户后续报告该目录 CMake 配置成功。不能据此证明本轮修改后的快照已构建。
- 尚存限制：当时 Linux 目录的快照和同步命令未保留。
- 日报/PPT 可使用要点：运行前需核对项目副本包含构建入口；只报告用户实际展示的配置结果。

## R3-02 — FDBus 5.4 头文件未包含定宽整数声明

- 日期：记录日期 2026-10-27；首轮构建事件日期未单独核实。
- 分类：第三方依赖兼容问题。
- 现象与用户反馈：用户提供的单线程构建日志首条编译诊断指向 `public/fdbus/IFdbMsgBuilder.h`，其中 `uint8_t` 未声明，诊断建议包含 `<cstdint>`；随后同一头文件中的 `int32_t` 也受影响。
- 证据：附件 `C:\Users\user\.codex\attachments\d98a27ca-41f6-48e3-948b-77641bd0c279\Pasted text.txt`；当前 `CMakeLists.txt` 在 FDBus 非 MSVC 构建参数中预包含 `cstdint`，没有修改第三方源文件。
- 原建议或实现：此前以并行构建末尾的 make `Error 1` 归因失败；拿到首条编译诊断后改为按缺失头文件定位。
- 根因：FDBus 5.4 提供的头文件使用定宽整数类型但未自行包含 `<cstdint>`，由首条诊断直接支持。
- 处理方式：在项目的 FDBus ExternalProject 和 transport 编译参数中预包含 `<cstdint>`，保持第三方源码不变。
- 验证结果：用户后续报告 FDBus target、`name_server` 和 `ipc_log_system` 构建成功；这是当时构建版本的用户报告，不覆盖本轮 C++ 整理。
- 尚存限制：本轮 Windows 执行端无 CMake/C++ 编译器，当前代码需要用户在 Ubuntu 重新构建。
- 日报/PPT 可使用要点：用首条编译诊断区分依赖头文件问题与后续级联错误；兼容改动留在项目构建层。

## R3-03 — 一次 worker flush 报告控制预留已满

- 日期：记录日期 2026-10-27；失败运行的事件日期未单独核实。
- 分类：运行异常 / 根因待核实。
- 现象与用户反馈：一次演示输出中 worker-b 报告 `flush=server flush control reserve is full`；随后的运行中两个 worker 都报告 `flush=confirmed`，服务端 `accepted=315 dropped=0 write_errors=0`。
- 证据：用户粘贴的 `worker-b.console`、`log_server.console`；当前 `log_server_main.cpp` 将 aggregator 的控制预留设为 `max_pending_flushes`，`handleFlush` 也先预留并在结束路径释放名额。
- 原建议或实现：之前将其作为容量竞争现象检查；现有对话无法确认失败时具体源代码版本、队列状态或精确触发分支。
- 根因：待核实。一次失败后有成功运行，但不能把“之后成功”当成根因已修复。
- 处理方式：保留双层有界预留、计数和拒绝诊断；本轮没有删除相关错误处理。
- 验证结果：历史版本出现过失败；后续用户运行报告成功。当前代码变化尚未在 Ubuntu 回归。
- 尚存限制：无失败时的队列占用、控制请求并发和准确版本差异。
- 日报/PPT 可使用要点：保留异常输出和后续成功证据；解释预留机制前需以本轮 Linux 回归验证。

## R3-04 — 本地调级演示不足以证明跨进程集中调级

- 日期：记录日期 2026-10-27；功能需求演进日期未单独核实。
- 分类：需求覆盖不足。
- 现象与用户反馈：早期本地 demo 使用 `Logger::setLevel` 展示运行时改级；用户指出当前目标是 FDBus 多进程系统。之后集中控制实现加入 worker 查询/设置协议和 server 控制命令。
- 证据：当前 `proto/log_protocol.proto` 的 `LevelControlRequest/Reply`，`src/client/fdbus_transport.cpp` 的 `onControlInvoke`，`src/server/log_server.cpp` 的 `startControl/onReply/onStatus`，`apps/log_server_main.cpp` 的命令解析；用户报告 `run_control_demo.sh` 输出 `Central level-control commands are in logs/log_server.console`。
- 原建议或实现：本地 `setLevel` 只验证 Logger 阈值变化，不覆盖跨进程命令链路。
- 根因：演示覆盖层级不足，不能把本地单进程结果当成集中控制验收。
- 处理方式：补上 `list/get-level/set-level` 命令、worker FDBus 回调和应用级确认；脚本逐项检查两个实例的设定和查询结果。
- 验证结果：用户在先前版本报告控制脚本完成且 worker flush 确认；当前修改后的版本待回归。
- 尚存限制：题目中 AT1–AT5 具体定义未提供，不能声称逐条覆盖。
- 日报/PPT 可使用要点：清晰区分 Logger 本地 API 调级与 FDBus 远程控制；只按实际脚本断言范围报告结果。

## R3-05 — 曾出现 LocalLogTarget 声明与实现不匹配

- 日期：记录日期 2026-10-27；错误日志事件日期未单独核实。
- 分类：AI 实现缺陷 / 代码副本版本差异待核实。
- 现象与用户反馈：用户贴出的 Linux 编译错误显示 `src/local_log_target.cpp` 将构造和成员函数定义为 `LogAggregator::LocalLogTarget`，并访问不存在的 `aggregator_`；编译器指出头文件声明的是 `LogAggregator` 类。
- 证据：用户提供的编译诊断；当前工作树的 `include/ipc_log/local_log_target.h` 将 `LocalLogTarget` 定义为 `typedef LogAggregator LocalLogTarget`，根 CMake 不编译 `src/local_log_target.cpp`。用户之后报告全部目标构建成功。
- 原建议或实现：错误日志表明当时 Linux 目录中的实现与头文件不一致；现有证据未保留该错误版本的完整源文件或变更差异。
- 根因：当时存在无效/不匹配的本地目标定义；它是否来自旧副本、未同步文件或具体生成过程待核实。
- 处理方式：当前版本直接复用 `LogAggregator`，删除额外转发目标实现路径；之后的用户构建报告证明当时目标可以编译。
- 验证结果：历史版本用户报告构建成功；本轮修改后尚待 Linux 构建。
- 尚存限制：无法比较错误版本与当时成功版本的完整源码差异。
- 日报/PPT 可使用要点：C++ 定义必须与声明和 CMake 编译单元一致；先核对实际源文件清单再推断修复。
- 后续变化：2026-10-28 删除演示入口后，`include/ipc_log/local_log_target.h` 已无使用方，随该头文件一并移除；以上是当时版本的记录，不代表当前工作树仍保留该别名。

## R3-06 — 控制演示脚本可能用旧日志提前发送 list

- 日期：记录日期 2026-10-27；首次失败事件日期未单独核实。
- 分类：AI 实现缺陷 / 演示时序问题。
- 现象与用户反馈：控制脚本等到 `log_server.console` 出现 worker-a 后便发送 `list`，但该日志文件可能含上次运行内容，导致 worker 尚未上线时报告 `Could not discover both workers`。
- 证据：用户提供的脚本片段；当前 `scripts/run_control_demo.sh` 在启动前截断 server 控制台日志，并等待本次运行中 worker-a、worker-b 两个模块日志；随后才发送 `list`。用户复制当前脚本后报告脚本到达 `Central level-control commands are in ...`，两 worker flush confirmed，server `accepted=307 dropped=0 write_errors=0`。
- 原建议或实现：早期等待条件只检查 worker-a 且未清空旧日志，存在陈旧日志满足条件的竞态。
- 根因：启动时序判断依赖可复用日志文件，而非本次运行的新鲜输出；等待条件也未覆盖两个 worker。
- 处理方式：启动前清空 server console 文件，等待两个 worker 都输出本次日志，再查询会话；保留 FIFO 和超时清理。
- 验证结果：用户报告该脚本运行到集中调级确认并正常结束，worker flush 和服务端统计无丢失/写错误。此结果属于脚本对应的历史版本。
- 尚存限制：本轮 source 还有其他改动，需再次在 Ubuntu 运行普通和控制 demo。
- 日报/PPT 可使用要点：以每次启动的新鲜信号作为就绪条件，不能只依赖残留文件；保留用户实际运行输出。

## R3-07 — 源码精简和注释首轮工作不足

- 日期：2026-10-27。
- 分类：协作过程 / 需求执行不充分。
- 现象与用户反馈：用户指出此前已实现功能，但规划中的代码精简和注释仍未完成；又指出首轮简单整理深度不够。
- 证据：用户消息“在本轮任务中，你漏掉的是代码精简和注释”；当前主规划 R1/R2 和源文件统计；当前状态 `git status` 显示项目文件均未跟踪，无法用 Git diff 还原首轮改动。
- 原建议或实现：此前做过局部 flush/轮转整理和少量注释，但没有覆盖两条完整链路，也没有逐文件验收。
- 根因：执行者把局部修补当成完成，未按规划覆盖全仓源文件并报告差异依据。
- 处理方式：本轮逐查 logger→worker→server→aggregator→Sink 与控制输入→server→worker→应答；去除重复进程身份状态、合并重复 reply 错误赋值与 server invalid-message 分支，并让队列项/回调采用移动传递；系统补充线程、锁、超时、ACK/flush 和退出注释。
- 验证结果：静态调用点和注释格式检查通过；27 个自研源码/构建/脚本文件当前 3,893 行、3,543 非空行、188 行独立注释。非空非注释行由基线 3,370 降为 3,355。C++ build/runtime 尚未运行。
- 尚存限制：用户需在 Ubuntu 对本轮代码重建并运行两套演示；当前文件均未跟踪，不能生成可信 Git diff 或精确归属历史改动。
- 日报/PPT 可使用要点：承认首轮覆盖不足；用基线与当前分项统计、具体状态去重和调用链注释说明纠偏，不把静态检查说成构建通过。

## R3-08 — 将“规划部分”误解为正式设计材料

- 日期：2026-10-27。
- 分类：需求理解偏差。
- 现象与用户反馈：用户指出规划者的规划还有部分未完成；执行者先把它理解为正式架构图/详细设计/PPT 材料，用户澄清本轮缺项是代码精简和注释。
- 证据：用户先后消息“规划者的部分规划你还没完成”和“在本轮任务中，你漏掉的是代码精简和注释”；主规划中的 R1/R2 项。
- 原建议或实现：错误地把“规划”联想到计划中的正式文档收尾，偏离当前代码审查任务。
- 根因：没有先对照唯一任务书中的未完成项，基于术语猜测了交付范围。
- 处理方式：回到 `docs/implementation-plan.md` 执行 R1/R2，再继续 R4/R5；不创建重复任务清单。
- 验证结果：用户明确纠正；代码精简和注释已在本轮落实，尚未进行 Linux 回归。
- 尚存限制：正式设计图/PPT仍按后续日程处理，不属于本轮遗漏项。
- 日报/PPT 可使用要点：遇到“规划未完成”先检查主计划具体勾选项，避免把后续文档任务提前替代当前代码工作。

## F1–F8 全量审查修复跟踪与日报素材

> 建档日期：2026-10-27。依据 [全量审查报告](docs/code-review-2026-10-27.md)；实施任务见 [主计划第 11 节](docs/implementation-plan.md)。
> 2026-10-27 已修改 F6/F7/F8 及结构精简 1–6（详见下方“本轮已修改”一节），状态为**已修改待验证**；F1/F2/F3/F4/F5 仍未修改，保持“待修复/待复现”。
> 2026-10-28 按用户决定移除跨进程 flush 协议，F7/F8 的原有实现被新的退出排空设计取代，详见文末“2026-10-28 移除 flush 协议”。
> 2026-10-28 另按用户要求删除 `local_log_demo`、`log_worker` 和旧演示脚本，演示代码拆到独立示例工程，F3 的旧脚本路径随之消失，详见文末“2026-10-28 示例工程拆分”。
> 后续保留发现、修改、验证各自真实日期。审查结论可被新证据修正，但不能先写“修复通过”再补证据。

| 编号 | 问题与当前状态 | 已有证据 | 后续须补充 | 素材阶段 |
|---|---|---|---|---|
| F1 / R10 | 关闭竞态可能跳过 FIFO 中已接受记录；已定位、**待修复** | 共享消费循环的静态时序推演；未运行 C++ 竞态用例；`async_queue.h` 仍只有 `closed()`，本轮未改 | 最小触发、关闭判断改动、修复前后输出/计数、实际验证 | 日志核心 |
| F2 / R11 | 按字节截断破坏 UTF-8；已定位、**待修复** | 合法中文按 128/4096 字节切片后严格解码失败；`logger.cpp` 仍按字节 `assign`，本轮未改 | 保持码点边界的改动、边界用例及 protobuf 上报结果 | 日志核心；跨进程复验归多进程阶段 |
| F3 / R12 | 演示脚本吞 server 失败；**旧脚本已删除，新脚本待 Ubuntu 验证** | 历史复现：临时 server exit 2、两个 worker exit 0，原始 run_demo.sh 实际 exit 0；2026-10-28 删除旧编排，`run_server.sh` 前台运行并保留状态，`run_demo.sh` 用 `exec` 继承 | 全成功、server 失败、示例失败、Ctrl-C 清理的实际退出码 | 多进程演示 |
| F4 / R13 | 服务未就绪就宣告 listening；已定位、**待修复** | SDK svc 注册异步，应用无实际绑定确认；未实测重复服务名 | SDK 状态依据、实现与启动成功/失败用例 | FDBus 联调 |
| F5 / R14 | 可写不可读文件的原有大小漏算；已定位、**待修复** | `rotating_file_sink.cpp` 仍用 `ifstream`+`tellg`，失败即当 0；本轮未改 | stat 处理、非 root 权限与轮转边界复验 | 日志核心 |
| F6 / R15 | 无符号参数接受负数；**已修改待验证**（2026-10-27） | 已加 `parseUnsignedDecimal`，拒绝空串、负号、溢出和尾随字符；语法检查通过 | Ubuntu 运行 `--count -1` 等边界用例 | 日志核心/入口参数 |
| F7 / R16 | 应用显式 flush 后 shutdown 再 flush；**2026-10-28 改为移除 flush 协议**，由退出排空取代 | 2026-10-27 曾统一为单次 `shutdown`；2026-10-28 删除 `flush`/`flushAsync` 与跨进程 flush 消息 | Ubuntu 验证正常退出排空、超时与退出码 | 本地部分归核心，worker 部分归多进程 |
| F8 / R17 | 异步回调同步等待/关闭的重入风险；**2026-10-28 随 flush 回调删除而收敛**，仅保留 `shutdown` 重入保护 | 消费线程调用 `shutdown` 直接返回 `already_stopped`/拒绝；无 flush 回调可重入 | Ubuntu 验证消费线程内调用 shutdown 不死锁、外部仍可关闭 | 日志核心/接口约束 |

### 2026-10-27 本轮已修改：结构精简 1–6 与 F6/F7/F8

> 状态：**已修改待验证**。改动只落在主项目 `C:\Users\32181\Desktop\HJM-fast`，未提交、未推送、未生成新的阶段快照。以下各项均无 Ubuntu 构建或运行证据。

**结构精简 1–6（用户授权“1–6 都修复”，性质为冗余合并，不新增功能）**

| 编号 | 改动 | 涉及文件/函数 | 修改理由 |
|---|---|---|---|
| 1 | 两份几乎相同的 FDBus 运行时引用计数合并为 `detail::acquireFdbusRuntime()` / `releaseFdbusRuntime()`，删除 `FdbusRuntime` 与 `ServerFdbusRuntime` 两个类 | `src/client/fdbus_transport.cpp:28`、`include/ipc_log/fdbus_transport.h:22`、`src/server/log_server.cpp:126` | 同一套 `FDB_CONTEXT` 启停逻辑写了两遍；合并后保留“SDK `destroy()` 不重置 `mInstance`，销毁后不能重启”的标记 |
| 2 | Sink 接口改为 `write(LogLevel, line)`，按等级选流，不再从格式化字符串反解等级 | `include/ipc_log/sink.h`、`src/sinks/console_sink.cpp`、`src/sinks/rotating_file_sink.cpp`、`src/server/log_aggregator.cpp` `outputReady`、`apps/log_server_main.cpp` `printCommandLine` | 原来靠字符串前缀判断 ERROR 走 stderr，属于隐式约定，格式一改就错 |
| 3 | 抽出 `LogServer::resolveSession(process_instance, deadline)`；`getWorkerLevel` 改为本地扫描后只对目标会话发一次 GET，不再先查询全部 worker；`setWorkerLevel` 复用同一函数 | `src/server/log_server.cpp:609`、`:229`、`:274` | 两处重复的“扫描 + 发现 + 匹配”逻辑；get-level 原实现会对每个 worker 都发一次请求 |
| 4 | 字段上限集中为 `kMaxModuleBytes` / `kMaxMessageBytes`，`LoggerConfig` 不再携带每实例上限；删除 `Logger` 构造校验、`LogAggregator::submit` 与 `FdbusLogTarget::submit` 的重复记录校验；保留服务端线上数据的 `validateRecord` | `include/ipc_log/log_record.h`、`include/ipc_log/logger.h`、`src/common/logger.cpp`、`src/server/log_server.cpp` `validateRecord` | 同一约束在三处重复且可配置漂移；信任边界只应保留在进程入口 |
| 5 | `ILogTarget::shutdown` / `LogAggregator::shutdown` / `FdbusLogTarget::shutdown` 统一返回 `FlushResult`；应用入口删除显式 `flush` 后再 `shutdown` 的重复收尾 | `include/ipc_log/log_target.h:69-71`、`src/server/log_aggregator.cpp`、`src/client/fdbus_transport.cpp`、`apps/log_worker_main.cpp`、`apps/local_log_demo.cpp` | 与 F7 同一处问题，一次改完；退出码现在对应同一次收尾结果 |
| 6 | 消息码集中为 `kLogRequestCode` / `kLevelControlCode`；删除未使用的 proto 回复枚举 `REPLY_DUPLICATE`、`REPLY_STOPPING`（枚举留空号） | `include/ipc_log/fdbus_transport.h:16-17`、`proto/log_protocol.proto` | 消息码原在两个 cpp 各写一遍；proto 中无人读取的枚举只会误导协议读者 |

**F6 / F7 / F8 具体记录**

- **F6（R15）参数负数**
  - 日期：2026-10-27；分类：实现缺陷（输入校验缺失），归属未核实。
  - 触发与根因：`Arguments::number` 直接把字符串交给 `std::strtoull`，`-1` 被解释为最大无符号值，进而可能变成超大队列容量或超长超时。
  - 修复前后：修复前无负号检查；修复后 `apps/app_support.h:32` 新增 `parseUnsignedDecimal`，拒绝空串、负号、溢出和尾随字符，`Arguments::number` 统一走该函数。`log_server_main.cpp` 的 `parseInstanceId` 也复用它。
  - 验证依据：`g++ -std=c++11 -fsyntax-only` 语法检查通过；未在 Ubuntu 实际运行 `--count -1`。
  - 状态：已修改待验证。素材阶段：日志核心/入口参数。

- **F7（R16）重复退出 flush**
  - 日期：2026-10-27；分类：接口设计冗余，归属未核实。
  - 触发与根因：worker 与本地 demo 先显式 `flush`、再 `shutdown`，而 `shutdown` 内部又发一次屏障；两次排空、两套结果，退出码只能看到前一次或后一次。
  - 修复前后：修复前 `ILogTarget::shutdown` 返回 `void`；修复后返回 `FlushResult`，`LogAggregator::shutdown` 先做最后一次 `flush(timeout)` 再关队列、join 线程；`FdbusLogTarget::shutdown` 返回 flush 结果后停止发送、销毁客户端、释放运行时。应用入口只保留一次 `target->shutdown(...)`，退出码取该结果。`LogServer::shutdown` 仍为 `void`，只负责端点清理，不再替聚合器 flush（输出排空由 `aggregator.shutdown` 负责）。
  - 验证依据：仅语法检查（worker/local demo）；server 侧未编译。未在 Ubuntu 验证重复关闭与超时路径。
  - 状态：已修改待验证。素材阶段：本地部分归日志核心，worker 部分归多进程。

- **F8（R17）回调线程重入**
  - 日期：2026-10-27；分类：接口边界缺陷，归属未核实。
  - 触发与根因：`flushAsync` 回调运行在聚合器消费线程；若回调内同步调用 `flush`/`shutdown`，会等待自己完成的屏障或 self-join，导致死锁或 `terminate`。
  - 修复前后：修复前只有实现上的隐式约束；修复后 `LogAggregator::flush`/`shutdown` 在检测到当前线程即消费线程时直接返回 `FlushStatus::Stopped` 并给出说明，头文件 `log_aggregator.h:43` 写明“回调运行在消费线程上，不能在回调里同步调用 flush/shutdown”。拒绝发生在改状态和排队之前，外部线程仍可正常关闭。
  - 验证依据：语法检查通过；未构造真实回调重入用例。
  - 状态：已修改待验证。素材阶段：日志核心/接口约束。

**本轮验证边界（不得外推）**

- 已执行：`g++ -std=c++11 -fsyntax-only -I include -I apps` 覆盖核心源码、`local_log_demo.cpp`、`log_worker_main.cpp`，退出码 0；检查未用标识符已清除。
- 未执行：`log_server_main.cpp` 因 Windows/MinGW 缺 `poll.h` 未参与检查；`log_server.cpp`、`fdbus_transport.cpp` 因缺 protoc 生成的 `log_protocol.pb.h` 和 Linux FDBus 头文件未编译；无 Ubuntu 构建、无多进程运行、无控制演示、无关闭竞态复验。
- 行数：自研源码约 3,603 行降至约 3,470 行（约 133 行、3.7%），仅作精简幅度参考，不作为验收指标。
- 未修改：`async_queue.h`（F1）、`logger.cpp` 截断逻辑（F2）、`scripts/run_demo.sh`（F3）、`LogServer::start` 绑定确认（F4）、`RotatingFileSink` 构造期大小读取（F5）。
- 可摘选素材：按用户授权合并重复的 FDBus 运行时、Sink 接口、会话解析与关闭路径，同时修正负数参数、重复 flush 和回调重入；结论限于静态检查，运行验证待 Ubuntu。

### 每项修复后由执行者补充的记录格式

- 编号与实际日期：F编号；发现、修改、验证分别记录。
- 分类与归属：实现缺陷/接口边界/协作理解；无法证明具体 AI 归属时写“归属未核实”。
- 触发与根因：输入、时序、影响、证据及对应文件/函数。
- 修复前后：改了什么、为什么这样改、为何没有扩大架构；注明实际文件/接口变化。
- 验证依据：主项目或快照版本、环境、命令/用户原始输出、实际结果和未覆盖项；补充最小失败用例的前后对照。
- 状态：待修复 / 已修改待验证 / 验证通过。无证据不得提升状态。
- 可摘选素材：1–2 句具体总结，注明适用阶段；只完成定位或修改时保留“待修复/待验证”，不能写成已通过验收。
- 素材用途：日报用简短结论；答辩可进一步展示缺陷位置、根因、修复前后和人工/AI 各自核对边界。不要杜撰人工检查、导师评价或运行成功。

### 已发生的需求边界校准：退出 flush 与运行时频繁 flush

- 日期：2026-10-27；分类：需求理解与风险优先级纠偏，不是新代码修复任务。
- 事实：用户指出题目只要求“退出时 flush”，没有要求平时周期发送。审查者随后核对 worker 调用点，确认显式 flush 位于生产循环结束后的收尾阶段。
- 原先偏差：把任意业务进程频繁发送 flush 当作主要场景，放大了当前共享缓存方案必须改造的理由。
- 校准结果：共享队列、共享重排缓存和退出屏障可以保留，说明跨窗口乱序边界即可；不增加题目未要求的进程级 flush 或周期 flush。
- 验证边界：依据题目原文与当前静态调用链，不代表 Ubuntu 运行验收。重复退出 flush 属于 F7；2026-10-27 已统一为单次 `shutdown`，状态为已修改待验证。
- 可摘选素材：结合原始需求和实际调用位置，纠正 AI 对使用场景的扩大解释，保留满足工期的简化架构，把修复重点转向真实可靠性缺陷。

### 2026-10-28 移除 flush 协议，改为日志系统退出时内部排空

- 日期：2026-10-28；分类：需求理解与接口简化；由用户明确决定“直接移除 flush 机制”，不是在原有屏障上继续打补丁。
- 需求依据：题目只要求“退出时 flush”，没有要求业务代码主动发起 flush，也没有要求运行期周期 flush。原设计把“退出时输出完”实现成了跨进程 flush 请求/应答，引入了业务侧可影响日志系统的入口。
- 原先偏差：worker 生产结束后显式 flush，`LogAggregator` 提供 `flushAsync`/`flush` 和队列控制预留，server 维护 pending flush 与回复占位。这些机制解决的是“业务要求日志系统立刻冲刷”，与题目要求不一致。
- 修改前后：
  - 协议：删除 `FlushRequest`、`LogRequest`、`RequestKind`；`kLogRequestCode` 直接以 `LogRecord` 为根消息；`LogReply` 只保留 `request_id/code/accepted/message`；旧字段号用 `reserved`。
  - 接口：`FlushResult`/`FlushStatus` 改为 `ShutdownResult`；`ILogTarget` 只保留 `submit`、`shutdown`、`stats`；聚合器删除 `flush`/`flushAsync` 和 `flush_mutex_`。
  - 队列：删除控制预留与 `tryPushControl`，`BoundedQueue` 只服务日志记录。
  - worker：发送线程在 `shutdown` 时排空本地队列，最多等待 `shutdown_timeout`（默认 10s）；超时则强制停止，未发出的记录计入 `dropped`，不再向服务端发 flush 屏障。
  - server：删除 pending flush 表、控制预留和 flush 处理分支；`onInvoke` 只反序列化 `LogRecord` 并校验入队。
  - 退出：`LogAggregator::shutdown` 停止接收 → 关闭队列 → 消费线程取完关闭前积压 → 强制释放重排窗口 → 刷新两个 Sink → join；`LogServer::shutdown` 只做端点清理。
  - 应用与脚本：`local_log_demo`、`log_worker`、`log_server` 只调用一次 `shutdown`，按 `ShutdownResult` 决定退出码；README 与 `run_demo.sh` 注释改为退出排空描述。
- 保留项：ACK 仍只表示服务端入队；worker 在途请求仍有超时与 `uncertain` 计数；Sink 仍在退出时刷新；`shutdown` 仍有消费线程重入保护。
- 语义边界：退出排空只覆盖已经进入对应本地队列/汇聚队列的记录；server 退出时仍在 FDBus 传输层、socket 或 worker 本地队列中的记录不在保证范围内；`kill -9`、崩溃和掉电仍可能丢失内存中的日志；stream 刷新不等于 `fsync`。
- 验证依据：`grep` 未发现 `FlushResult|flushAsync|max_pending_flushes|control_reserve|tryPushControl|REQUEST_FLUSH|FlushRequest|REPLY_FLUSHED|protocol::LogRequest` 残留；核心源码与 `local_log_demo.cpp` 的 `g++ -std=c++11 -fsyntax-only` 退出码 0；同口径自研源码约 3,470 行降至 3,135 行。`log_server.cpp`、`fdbus_transport.cpp`、`log_server_main.cpp` 未在 Windows 编译，未执行 Ubuntu 构建、双 worker 演示或退出排空复验。
- 状态：已修改待验证。F7 的“重复 flush”问题因接口删除而消失，F8 的 flush 回调重入面随之收敛；两者都不能据此写成运行验证通过。
- 可摘选素材：按题目“退出时输出完”的原意，删掉业务可触发的跨进程 flush 协议，把收尾统一为一次内部排空；减少协议分支、控制预留和 pending 状态，同时保留 ACK、超时、丢弃计数与资源释放。

### 2026-10-28 拆分独立示例工程，删除演示入口与旧脚本

- 日期：2026-10-28；分类：工程边界与协作纠偏；由用户明确指出“测试 demo 和脚本不要和日志系统混在一起，一个是示例、一个是项目”。
- 原先偏差：`apps/local_log_demo.cpp`、`apps/log_worker_main.cpp` 与产品入口放在同一目录、同一根 CMake 中，演示参数和演示流程混进产品构建；`scripts/run_demo.sh`、`scripts/run_control_demo.sh` 还承担多进程编排，导致脚本职责和日志系统职责重叠。
- 修改前后：
  - 产品：根工程只保留 `log_server` 入口；`local_log_demo`、`log_worker` 两个目标删除，根 `CMakeLists.txt` 不再生成任何演示程序。
  - 示例：新增独立 CMake 工程 `examples/fdbus-console-demo/`，通过 `add_subdirectory` 把本仓库作为依赖引入，只链接 `ipc_log_transport`；示例源码只使用公开头文件，不在产品源码目录内。
  - 输入方式：示例进程持续读取 stdin，每行按 `<等级> <模块> <正文>` 调用一次日志库，另支持 `:level`、`:stats`、`:help`、`:quit` 本地命令；启动时打印自身 `instance=`，供集中调级使用。
  - 脚本：删除 `run_demo.sh`、`run_control_demo.sh` 的旧编排，改为 `scripts/build.sh`、`scripts/run_server.sh`、`scripts/run_demo.sh`。`run_server.sh` 后台拉起 name_server、前台运行 log_server；两个示例各占一个终端。
  - 入口：新增 `FdbusLogTarget::processInstance()`，使示例能打印服务端 `list/get-level/set-level` 所需的实例号。
- 与 F3 的关系：旧脚本“后台 worker 掩盖 server 退出码”的路径随旧编排删除而消失；`run_server.sh` 前台运行 log_server 并由 trap 保留退出状态，`run_demo.sh` 用 `exec` 继承客户端状态。该结论属于静态脚本检查，仍需 Ubuntu 实测失败/中断场景。
- 验证依据：`g++ -std=c++11 -fsyntax-only -I include examples/fdbus-console-demo/demo_client.cpp` 退出码 0；核心 cpp 同口径语法检查退出码 0；未执行完整 FDBus/Protobuf 构建，未在 Ubuntu 运行三终端示例，未验证集中调级或退出排空。
- 状态：已修改待验证。
- 可摘选素材：按“示例与项目分离”的要求，把演示代码移出产品工程、改为独立 CMake 子项目，并用三个终端分别运行日志系统和两个示例进程；演示不再进入产品目标，产品也不依赖演示脚本。
