# IPC 日志系统

## 目标

在 Linux 上实现一个基于 FDBus 5.4.0 的多进程日志系统：业务进程把日志异步上报到主进程，主进程汇聚、有限窗口排序，并输出到控制台和轮转文件。总体架构以 [docs/architecture-baseline.md](docs/architecture-baseline.md) 为准。

## 项目结构

```text
apps/                      产品入口：log_server
include/ipc_log/           日志核心与传输层头文件
src/                       核心与传输层实现
proto/                     Protobuf 协议
examples/fdbus-console-demo/  独立示例工程，不属于产品源码
scripts/                   构建、日志系统启动、示例客户端启动
```

日志系统本身由 `ipc_log_core`、`ipc_log_transport` 和 `log_server` 组成。示例工程有自己的 `CMakeLists.txt`，只链接日志系统库，不加入根工程的构建目标。

## 数据流

```text
示例进程（demo-a / demo-b）
  -> Logger（等级过滤、记录构造、非阻塞入队）
  -> FdbusLogTarget（本地有界队列 + 后台发送线程）
  -> FDBus
  -> log_server（校验、汇聚队列、有限窗口排序）
  -> 控制台 + 轮转文件
```

日志格式为 `[毫秒时间戳][等级][PID][模块][正文]`。接收 ACK 只表示进入服务端汇聚队列，不代表已经写出。业务代码不触发 flush：示例退出时排空本地队列，log_server 退出时排空汇聚队列并刷新两个 Sink。

## 边界与约束

- 目标环境是 Linux，C++11，CMake；复用项目内 `fdbus-v5.4.0/fdbus-v5.4.0` 和 Protobuf。Windows 只用于编辑项目。
- 跨进程日志链路使用 FDBus client/server，不使用 FDBus 自带的 `logsvc`/`logviewer`。
- 支持 DEBUG/INFO/WARN/ERROR，进程独立动态等级。
- 业务线程只过滤、构造并向有界队列提交，不等待 I/O、网络或 ACK；队列满丢弃新日志并计数。
- 主进程只保证有限窗口内的顺序；队列、消息长度、在途请求、排序缓存和文件保留数都有上限。
- 文件输出按大小轮转；远程日志实现、全局配置中心、自动重传去重和断电零丢失不在首版范围。

## 构建

```bash
bash scripts/build.sh
```

构建目录为 `build-demo/`，生成：

```text
build-demo/fdbus_console_demo
build-demo/ipc_log_project/log_server
build-demo/ipc_log_project/_deps/fdbus-build/name_server
```

只构建产品也可以：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ipc_log_system --parallel
```

## 运行

需要三个终端。

**终端 1：日志系统**

```bash
bash scripts/run_server.sh
```

脚本后台拉起 name_server，前台运行 log_server。日志直接显示在这个终端，同时可以输入 `list`、`get-level`、`set-level` 控制命令。

**终端 2 和 3：两个示例客户端**

```bash
bash scripts/run_demo.sh demo-a
bash scripts/run_demo.sh demo-b
```

启动后按固定格式持续输入：

```text
INFO orders hello world
WARN payment charge retried
ERROR inventory stock is empty
```

格式为 `<等级> <模块> <正文>`，等级大小写不敏感，模块不能含空格，正文可以含空格。`:help`、`:stats`、`:level DEBUG`、`:quit` 是本地命令。

集中调级在终端 1 执行：

```text
list
set-level <demo-a 的 instance> DEBUG
set-level <demo-b 的 instance> ERROR
```

两个示例进程的等级互不影响。示例启动时会打印自己的 `instance=`。

## 当前推进安排

日志核心、FDBus 多进程和集中动态调级已实现。2026-10-27 完成审查后的结构精简，2026-10-28 移除跨进程 flush 协议，并把演示代码拆到独立示例工程。当前版本的 Linux 构建和运行结果待 Ubuntu 验证；正式架构图和答辩材料按项目计划后续整理。
