#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build}"
log_dir="${2:-${project_root}/logs}"
worker_count="${3:-100}"
name_server="${build_dir}/_deps/fdbus-build/name_server"
server_bin="${build_dir}/log_server"
worker_bin="${build_dir}/log_worker"

for executable in "$name_server" "$server_bin" "$worker_bin"; do
    if [[ ! -x "$executable" ]]; then
        printf 'Missing executable: %s\nBuild the project first with cmake --build %s --target ipc_log_system --parallel.\n' \
            "$executable" "$build_dir" >&2
        exit 1
    fi
done

#确保输出目录存在；name service 与主服务先于两个 worker 启动。
mkdir -p "$log_dir"
name_pid=""
server_pid=""
worker_pids=()

cleanup() {
    #信号或任一 worker 退出时按 worker、server、name_server 顺序收尾并等待子进程。
    local status=$?
    trap - INT TERM EXIT
    for pid in "${worker_pids[@]}"; do
        kill -INT "$pid" 2>/dev/null || true
    done
    for pid in "${worker_pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    if [[ -n "$server_pid" ]]; then
        kill -INT "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if [[ -n "$name_pid" ]]; then
        kill -TERM "$name_pid" 2>/dev/null || true
        wait "$name_pid" 2>/dev/null || true
    fi
    printf 'Demo stopped. Logs are in %s\n' "$log_dir"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$name_server" >"${log_dir}/name_server.log" 2>&1 &
name_pid=$!
sleep 1
kill -0 "$name_pid"

#stdin 保留为控制入口；无交互输入时服务端检测 EOF 后继续提供日志服务。
"$server_bin" --file "${log_dir}/aggregated.log" <&0 >"${log_dir}/log_server.console" 2>&1 &
server_pid=$!
sleep 1
kill -0 "$server_pid"

"$worker_bin" --name worker-a --count "$worker_count" --interval-ms 50 >"${log_dir}/worker-a.console" 2>&1 &
worker_pids+=("$!")
"$worker_bin" --name worker-b --count "$worker_count" --interval-ms 50 >"${log_dir}/worker-b.console" 2>&1 &
worker_pids+=("$!")

printf 'Started name_server, log_server, worker-a, and worker-b (each worker: %s records).\n' "$worker_count"
#等待两个 worker 结束。
for pid in "${worker_pids[@]}"; do
    wait "$pid"
done
worker_pids=()

#所有 worker 收尾后通知 server 正常退出；服务端在退出路径排空汇聚队列并刷新输出。
if [[ -n "$server_pid" ]]; then
    kill -INT "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=""
fi

#停止 name service。
if [[ -n "$name_pid" ]]; then
    kill -TERM "$name_pid" 2>/dev/null || true
    wait "$name_pid" 2>/dev/null || true
    name_pid=""
fi
