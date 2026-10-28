# IPC 日志系统：FDBus 多进程阶段搬运代码（2026-10-28）

本目录是 2026-10-28 多进程阶段的搬运工作副本，覆盖日志核心、FDBus 多进程汇聚、集中动态调级和退出排空。它从首版交付后持续同步同一阶段的后续修正，当前代码对应主项目提交 `3693431`；不是另一套开发目录。与 2026-10-27 的 `日志核心-v2` 包相比，本阶段新增传输层、服务端、worker、协议和演示脚本，并移除了跨进程 flush 协议。

## 按实际记录逐步推进

- 10 月 27 日交付日志核心阶段，见 `每日交付/2026-10-27-日志核心-v2/` 和根目录 `2026-10-27-工作日志.md`。
- 10 月 28 日提交 `6381dc0` 移除跨进程 flush 协议，提交 `a1872da` 整理首版多进程阶段代码。
- 随后提交 `682af2f` 修正 FDBus context 宏调用；提交 `3693431` 固定 Shell 脚本 LF 行尾。

以上顺序以 HJM-fast Git 记录为准，不倒填日报日期、不伪造提交历史。10/28 日报尚未编写，本搬运目录当前不包含日报。

## 目录

```text
apps/                 进程入口与公共参数
include/ipc_log/      记录、Logger、队列、排序、Sink、传输与服务端接口
src/                  上述模块实现
proto/                Protobuf 协议
scripts/              构建与演示脚本
.gitattributes        固定脚本行尾为 LF
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

用户提供的 Ubuntu 输出中，`./build/local_log_demo --count 10` 摘要为 `shutdown=drained accepted=31 dropped=0 write_errors=0`。同轮多进程和集中调级演示因脚本第 2 行 `set -o pipefail` 行尾解析失败而提前退出；随后提交 `3693431` 加入 LF 行尾规则，但修正后的脚本尚未在 Ubuntu 重跑。

当前交付目录已同步提交 `682af2f` 的 FDBus context 修正和 `3693431` 的 `.gitattributes`。尚无当前版本完整构建成功输出；双 worker、集中调级、溢出/轮转和退出排空仍需目标环境重新验证。F1–F5 也仍按主计划跟踪，本包不代表全量审查问题已经全部修复。
