# IPC 日志核心阶段

## 本阶段范围

本包展示单进程日志核心：Logger 四级过滤、记录格式化、有界异步队列、有限窗口重排、控制台输出、按大小轮转的文件输出、flush 和本地多生产者演示。

跨进程 FDBus 上报、服务端汇聚和集中动态调级不包含在本阶段包中；后续按第三天计划交付与专项验收。最终项目仍须通过 FDBus 完成跨进程通信。

## 依赖和构建

依赖 CMake 3.16 或更新版本、C++11 编译器和线程库（Threads）。本阶段 CMake 不查找 Protobuf，也不构建 FDBus。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target local_log_demo --parallel
./build/local_log_demo --count 10
```

运行后日志写入控制台和默认文件 `logs/local-demo.log`。文件超过大小阈值时按 `*.1`、`*.2` 等编号切割，默认保留 3 个备份。

## 演示参数

- `--count N`：每个生产线程生成的记录轮数，默认 20。
- `--producers N`：生产线程数，范围 1–64，默认 1。
- `--level DEBUG|INFO|WARN|ERROR`：初始最低输出等级，默认 INFO。
- `--queue N`：汇聚记录队列容量，默认 8192。
- `--file PATH`：日志文件路径，默认 `logs/local-demo.log`。
- `--max-bytes N`、`--backups N`：文件大小上限和备份数。

输出格式为 `[毫秒时间戳][等级][PID][模块][正文]`。`flush` 确认队列屏障之前的 Sink 写入和 C++ stream flush，不承诺 `fsync` 或掉电持久化；并发记录使用有限窗口排序，不保证严格全序。

## 验证状态

本修订快照的源码来自主项目核心依赖白名单。包内 23 个文件的哈希见 [SHA256SUMS.txt](SHA256SUMS.txt)，该清单自身不参与哈希；18 个核心源码均与当前主项目哈希一致。复核原快照时发现 `apps/app_support.h` 少了主项目现有的四处解释性注释，因此在本修订快照中同步该文件；可执行语句未变化，原快照保留在 `每日交付/2026-10-27-日志核心/`。包内 CMake 和本 README 为阶段适配文件。当前仅完成文件清单、依赖闭包与哈希静态核对，独立 CMake 构建和 `local_log_demo` 运行待 Ubuntu 复验。

构建完成并人工确认运行输出后，再将本阶段标记为验收通过。
