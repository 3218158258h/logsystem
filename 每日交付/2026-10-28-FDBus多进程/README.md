# IPC 日志系统：FDBus 多进程阶段快照（2026-10-28）

本包是 2026-10-28 的完整工程源码快照，覆盖日志核心、FDBus 多进程汇聚、集中动态调级和退出排空。与 2026-10-27 的 `日志核心-v2` 包相比，本包新增传输层、服务端、worker、协议和演示脚本，并移除了跨进程 flush 协议。

## 目录

```text
apps/                 进程入口与公共参数
include/ipc_log/      记录、Logger、队列、排序、Sink、传输与服务端接口
src/                  上述模块实现
proto/                Protobuf 协议
scripts/              构建与演示脚本
CMakeLists.txt        完整工程构建配置
```

## 依赖

- Linux（Ubuntu 22.04/24.04 或 WSL2 Ubuntu）
- GCC/G++ 支持 C++11
- CMake >= 3.16
- Protobuf 开发包与 `protoc`（本包按 3.21.12 整理）
- FDBus 5.4.0 源码

本包不复制第三方 FDBus 源码。默认在包根目录查找 `fdbus-v5.4.0/fdbus-v5.4.0`；也可以显式指定：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFDBUS_SOURCE_DIR=/path/to/fdbus-v5.4.0/fdbus-v5.4.0
```

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ipc_log_system --parallel
```

生成 `local_log_demo`、`log_server`、`log_worker`，并在构建目录的 `_deps/fdbus-build` 下生成 FDBus 库和 `name_server`。

## 运行

单进程本地演示：

```bash
./build/local_log_demo --count 10 --producers 2 --file logs/local-demo.log
```

多进程汇聚演示：

```bash
bash scripts/run_demo.sh build logs 100
```

集中调级演示：

```bash
bash scripts/run_control_demo.sh build control-logs 120
```

`run_demo.sh` 启动 name_server、log_server 和 worker-a/worker-b；`run_control_demo.sh` 额外执行 `list`、`get-level`、`set-level`。

## 语义与边界

- 日志格式：`[毫秒时间戳][等级][PID][模块][正文]`。
- 业务线程只做等级过滤、记录构造和一次有界入队尝试，不等待网络、ACK 或磁盘。
- ACK 只表示记录已进入服务端汇聚队列，不代表已经写出。
- 主进程按有限窗口排序，不承诺跨进程严格全序；迟到记录保留并计数。
- 文件按大小轮转，备份数量有上限。
- 业务代码不触发 flush。worker 退出时排空本地队列，log_server 退出时排空汇聚队列、释放排序窗口并刷新两个 Sink；退出等待受 `shutdown_timeout` 限制。
- 退出排空不覆盖仍在传输层或内存中的记录，也不等于 `fsync` 或掉电持久化。

## 验证状态

本包在 Windows 工作区整理，仅完成源码级静态检查与标识符清理核对，未在 Ubuntu 构建或运行。构建、双 worker 演示、控制演示、溢出/轮转和退出排空均需在目标环境验证后再引用结果。
