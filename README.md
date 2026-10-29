# IPC 日志系统

基于 FDBus 的 Linux 多进程日志系统。业务进程异步上报日志，主进程统一汇聚、按时间戳在有限窗口内排序，并输出到控制台和轮转文件。

支持 DEBUG、INFO、WARN、ERROR 四级日志、毫秒时间戳、按实例动态调级，以及有界队列的丢弃计数和退出排空。

## 源码结构

- `src/common/`：日志记录、Logger 和共用 FDBus 运行时。
- `src/client/`：业务端异步排队、发送和等级控制。
- `src/server/`：日志接收、汇聚和排序。
- `src/sinks/`：日志格式化、控制台与轮转文件输出。

## 环境依赖

- Linux、支持 C++11 的 GCC/G++、CMake 3.16 及以上。
- Protobuf 开发库与 protoc。
- FDBus 5.4.0 源码，默认放在 `fdbus-v5.4.0/fdbus-v5.4.0/`，该目录下应包含 `cmake/CMakeLists.txt`。

## 构建

在项目根目录执行：

```bash
bash scripts/build.sh
```

构建结果放在 `build-demo/`，包含日志服务 `log_server`、名字服务 `name_server` 和示例客户端 `fdbus_console_demo`。

FDBus 源码在其他位置时，可先指定源码路径再构建：

```bash
cmake -S examples/fdbus-console-demo -B build-demo \
  -DCMAKE_BUILD_TYPE=Release \
  -DFDBUS_SOURCE_DIR=/path/to/fdbus-source
cmake --build build-demo --target fdbus_console_demo log_server --parallel
```

## 运行

以下命令均在项目根目录执行。

终端 1：启动名字服务和日志服务。

```bash
bash scripts/run_server.sh
```

终端 2：启动业务实例 A。

```bash
bash scripts/run_demo.sh demo-a
```

终端 3：启动业务实例 B。

```bash
bash scripts/run_demo.sh demo-b
```

在业务终端按 `<等级> <模块> <正文>` 输入日志，例如：

```text
INFO orders order created
WARN payment retry requested
ERROR inventory stock unavailable
```

## 动态调级

在日志服务终端执行，`<instance>` 替换为客户端启动时打印的实例号：

```text
list
get-level <instance>
set-level <instance> DEBUG
```

修改只作用于对应实例。调为 DEBUG 后，可在该业务终端输入 `DEBUG orders trace message` 查看效果。客户端也支持 `:level DEBUG`、`:stats` 和 `:help`。

## 日志与退出

日志显示在终端 1，同时写入 `logs/aggregated.log`；文件默认按 5MB 轮转，保留 3 份备份。

先在两个业务终端输入 `:quit`（或按 Ctrl-D），再在日志服务终端按 Ctrl-C。客户端和日志服务分别排空队列，服务退出时刷新输出；启动脚本随后关闭名字服务。

## 压力测试

```bash
bash scripts/run_stress_test.sh
```

两个进程各以 0ms 间隔发送 10000 条日志，四种等级轮流发送。脚本自动清理专用测试目录、后台运行并收尾，最后从文件统计丢失率；结果保存在 `logs/stress-test/`。

压测默认每个客户端队列 16384 条、服务端汇聚队列 8192 条，可分别调整：

```bash
bash scripts/run_stress_test.sh --client-queue 16384 --server-queue 32768
```

结果同时打印两端的丢弃、拒绝和结果未知计数，用于判断丢失发生在哪一段。
