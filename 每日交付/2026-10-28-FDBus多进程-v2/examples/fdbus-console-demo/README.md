# FDBus 控制台日志示例

本目录是独立示例工程，不属于日志系统的产品代码。产品工程只构建 `log_server`；本示例用自己的 CMakeLists 通过 `add_subdirectory` 引入日志系统，只链接 `ipc_log_transport`，演示两个业务进程如何：

- 持续从终端读取日志并调用日志库；
- 经 FDBus 把日志送到 log_server；
- 接受 log_server 的集中等级控制。

## 构建

在项目根目录执行：

```bash
bash scripts/build.sh
```

构建目录为 `build-demo/`，产物：

```text
build-demo/fdbus_console_demo
build-demo/ipc_log_project/log_server
build-demo/ipc_log_project/_deps/fdbus-build/name_server
```

也可以只构建示例：

```bash
cmake -S examples/fdbus-console-demo -B build-demo -DCMAKE_BUILD_TYPE=Release
cmake --build build-demo --target fdbus_console_demo log_server --parallel
```

## 运行：三个终端

**终端 1：日志系统**

```bash
bash scripts/run_server.sh
```

脚本在后台拉起 name_server，前台运行 log_server。日志会直接显示在这个终端；同时可以输入控制命令。

**终端 2：demo-a**

```bash
bash scripts/run_demo.sh demo-a
```

**终端 3：demo-b**

```bash
bash scripts/run_demo.sh demo-b
```

启动后每个 demo 会打印：

```text
demo-a ready pid=12345 instance=67890 service=svc://ipc_log_service
input: LEVEL MODULE MESSAGE      e.g. INFO orders hello world
local: :level DEBUG | :stats | :help | :quit
```

## 输入格式

```text
<等级> <模块> <正文>
```

例如：

```text
INFO orders hello world
WARN payment charge retried
ERROR inventory stock is empty
DEBUG cache lookup miss
```

等级必须是 `DEBUG`、`INFO`、`WARN`、`ERROR` 之一，大小写不敏感。模块不能包含空格，正文可以包含空格。

本地命令：

```text
:level DEBUG   只改当前 demo 的本地阈值
:stats         查看本进程过滤/提交/拒绝计数
:help          查看格式
:quit          排空本地队列并退出
```

也可以按 `Ctrl-D` 结束输入。

## 集中调级

在终端 1（log_server）输入：

```text
list
get-level <instance>
set-level <instance> DEBUG
```

`instance` 使用 demo 启动时打印的 `instance=` 数字。两个 demo 各自独立：

```text
list
get-level 67890
set-level 67890 DEBUG
get-level 67891
set-level 67891 ERROR
```

修改后，只有对应 demo 的后续输入会受影响；已经入队的记录继续处理。

## 边界

- 示例进程必须在 name_server 和 log_server 启动后再启动；否则记录可能因离线而超时或丢弃。
- 日志只保证有限窗口排序，不承诺跨进程严格全序。
- 接收 ACK 只表示进入服务端汇聚队列，不代表已经写出。
- `:quit` / `Ctrl-D` 会排空本地队列；直接 `kill -9` 不保证。

## 常见问题

- **启动后提示连接失败或记录超时**：先确认终端 1 的 `name_server` 和 `log_server` 已经启动，再启动示例。
- **`list` 看不到刚启动的示例**：等示例打印 `ready` 并产生第一条日志后重试；服务端只登记已连接的会话。
- **`set-level` 找不到实例**：使用示例启动时打印的 `instance=` 数字，不是 PID。
- **两个示例等级互相影响**：`set-level` 只作用于指定实例；用 `get-level` 分别确认。
- **找不到可执行文件**：先在项目根目录执行 `bash scripts/build.sh`，不要直接运行示例目录。
