# IPC 日志系统：FDBus 多进程快照 v2（2026-10-28）

本包是 2026-10-28 的完整工程源码快照，覆盖日志核心、FDBus 多进程汇聚、集中动态调级和退出排空。相对同日第一版快照，本版删除了 `local_log_demo`、`log_worker` 两个演示入口和旧演示脚本，把演示拆到独立示例工程 `examples/fdbus-console-demo/`。

## 包内结构

```text
apps/                         产品入口：log_server 及公共参数/信号辅助
include/ipc_log/              日志核心与传输层头文件
src/                          核心与传输层实现
proto/                        Protobuf 协议
examples/fdbus-console-demo/  独立示例工程，不属于产品目标
scripts/                      构建、日志系统启动、示例客户端启动
docs/                         架构基线与交付计划
项目架构.md                   当前实现结构、线程模型和已知边界
执行计划.md                   分日执行安排与当前状态
2026-10-28-工作日志.md        当日四部分工作日志
Windows提交命令.bat           Windows 下的暂存与提交脚本
```

产品工程默认只构建 `ipc_log_core`、`ipc_log_transport`、`log_server`；示例工程有自己的 `CMakeLists.txt`，通过 `add_subdirectory` 把本包当作依赖引入。

## 构建

需要 Linux、CMake、C++11、Protobuf 3.21.12 和 FDBus 5.4.0 源码。将 FDBus 源码放在包内 `fdbus-v5.4.0/fdbus-v5.4.0`，或配置 `FDBUS_SOURCE_DIR`。

**构建产品与示例（推荐，供演示使用）**

```bash
bash scripts/build.sh
```

生成：

```text
build-demo/fdbus_console_demo
build-demo/ipc_log_project/log_server
build-demo/ipc_log_project/_deps/fdbus-build/name_server
```

**只构建产品**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFDBUS_SOURCE_DIR=/path/to/fdbus-v5.4.0/fdbus-v5.4.0
cmake --build build --target ipc_log_system --parallel
```

## 运行

需要三个终端。

**终端 1：日志系统**

```bash
bash scripts/run_server.sh
```

脚本后台拉起 name_server，前台运行 log_server；日志显示在该终端，同时可以输入 `list`、`get-level`、`set-level`。

**终端 2、3：两个示例客户端**

```bash
bash scripts/run_demo.sh demo-a
bash scripts/run_demo.sh demo-b
```

启动后按 `<等级> <模块> <正文>` 持续输入：

```text
INFO orders hello world
WARN payment charge retried
ERROR inventory stock is empty
```

本地命令：`:help`、`:stats`、`:level DEBUG`、`:quit`。示例启动时会打印自己的 `instance=`。

**集中调级（终端 1）**

```text
list
set-level <demo-a 的 instance> DEBUG
set-level <demo-b 的 instance> ERROR
```

两个示例进程的等级互不影响。

## 关键语义

- 日志格式：`[毫秒时间戳][等级][PID][模块][正文]`。
- 业务线程只做过滤、记录构造和有界入队，不等待网络、磁盘或 ACK。
- 接收 ACK 表示进入服务端汇聚队列，不代表已经写出。
- 队列满受控丢弃并计数；不自动扩容。
- 业务代码不调用 flush；示例退出时排空本地队列，log_server 退出时排空汇聚队列并刷新两个 Sink。
- 退出排空只覆盖已进入对应队列的记录；崩溃、`kill -9` 或掉电仍可能丢失内存中的日志。

## Windows 提交

在 Windows 资源管理器中双击 `Windows提交命令.bat`，或在命令行执行：

```bat
Windows提交命令.bat
```

脚本会定位所在 Git 仓库根目录，只暂存源码、示例、脚本、README、架构文档、执行计划和 10/28 日报，然后用题目要求的 `[关键字] 英文描述` + `[what]/[why]/[how]` 格式提交。脚本不会自动 push；推送前请先核对 `git diff --cached`。

## 验证状态

已执行（Windows，静态）：

- 示例客户端 `g++ -std=c++11 -fsyntax-only -I include` 退出码 0。
- 核心 cpp 同口径语法检查（`-Wall -Wextra`）退出码 0。
- 三个 Shell 脚本 `bash -n` 通过。
- 包内源码、示例、脚本和 CMake 与主项目逐文件比对一致；包内 README 和交付说明是交付版文字，与根目录对应文件有意不同。

未执行：

- 本包在 Ubuntu 的完整配置、构建和运行。
- `log_server`、`fdbus_transport.cpp`、`log_server.cpp` 的编译：Windows 缺少 protoc 生成头文件和 Linux FDBus 头文件。
- 三终端演示、集中调级、断连/超时、队列溢出、文件轮转、退出排空的实际运行。

因此本包只能声明“源码已整理、静态检查通过”，不能声明“构建通过”或“运行验收通过”。

## 已知未修复项

- F1：关闭竞态可能跳过队列中已接受的记录。
- F2：按字节截断可能破坏 UTF-8。
- F3：新脚本的退出码传播尚未在 Ubuntu 复验。
- F4：服务端绑定未真正可用就报告 listening。
- F5：轮转文件已有内容的原大小读取可能失败并当作 0。
