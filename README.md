# IPC 日志系统

## 目标

在 Linux 上实现一个基于 FDBus 5.4.0 的多进程日志系统：worker 进程将日志异步上报到主进程，主进程汇聚、有限窗口排序，并输出到控制台和轮转文件。总体架构以 [docs/architecture-baseline.md](docs/architecture-baseline.md) 为准。

## 项目框架

```text
业务线程
  -> Logger（等级过滤、记录构造、非阻塞入队）
  -> ILogTarget
       ├─ LocalLogTarget（单进程阶段）
       ├─ FdbusLogTarget（worker 后台发送）
       └─ LogAggregator（主进程本地/远端统一入口）
              -> 有界队列 -> 排序窗口 -> ConsoleSink + RotatingFileSink

FDBus: name_server + log_server + 至少两个 log_worker
协议: Protobuf；服务名 svc://ipc_log_service
```

模块分层：`ipc_log_core` 放记录、Logger、队列、排序和 Sink，不依赖 FDBus；`ipc_log_transport` 负责 Protobuf/FDBus 客户端与服务端适配；`apps/` 负责进程装配、演示参数与退出流程。

## 边界与约束

- 目标环境是 Linux，C++11，CMake；复用项目内 `fdbus-v5.4.0/fdbus-v5.4.0` 和 Protobuf。Windows 只用于编辑项目，Linux 构建/运行由用户验证。
- 跨进程日志链路必须使用 FDBus client/server 异步请求/应答，禁止用 socket 等替代；不使用 FDBus 自带的 `logsvc`/`logviewer`。
- 支持 DEBUG/INFO/WARN/ERROR，进程独立动态等级；日志格式为 `[毫秒时间戳][等级][PID][模块][正文]`。
- 业务线程只过滤、构造并向有界队列提交，不等待 I/O、网络或 ACK；队列满丢弃新日志并计数。
- worker 最多一个未确认请求；接收 ACK 只表示服务端入队。业务代码不触发 flush：worker 退出时把本地队列发完，log_server 退出时排空汇聚队列并刷新两个 Sink。
- 主进程统一排序，但只保证有限窗口内的顺序；队列、消息长度、在途请求、排序缓存和文件保留数都有上限。
- 文件输出按大小轮转；远程日志实现、全局配置中心、自动重传去重和断电零丢失不在首版范围。

## 当前推进安排

第一版日志、汇聚和集中动态调级功能已实现。2026-10-27 完成审查后的结构精简，2026-10-28 移除跨进程 flush 协议，改为退出时由日志系统内部排空。Windows 工作区缺少 Linux FDBus 头文件、Protobuf 生成环境和 `poll.h`，只完成核心源码与本地演示的语法检查；完整构建和运行结果待 Ubuntu 验证。正式架构图和答辩材料按项目计划后续整理。

## 构建与运行

在 Ubuntu 环境安装好项目所需的 CMake、C++ 编译器、Protobuf 开发包后，从项目根目录运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ipc_log_system --parallel
bash scripts/run_demo.sh build logs 100
```

`run_demo.sh` 启动 name_server、log_server 和两个 log_worker。集中调级演示使用：

```bash
bash scripts/run_control_demo.sh build control-logs 120
```

本地单进程链路可用 `./build/local_log_demo --count 10` 演示。FDBus 5.4.0 源码位于 `fdbus-v5.4.0/fdbus-v5.4.0`，CMake 在构建目录下单独编译其传输库与 name_server；Protobuf C++ 源文件由构建生成。
