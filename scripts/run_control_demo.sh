#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build}"
log_dir="${2:-${project_root}/logs}"
worker_count="${3:-120}"
fifo="${log_dir}/log_server.stdin"
demo_pid=""

mkdir -p "$log_dir"
if [[ -e "$fifo" ]]; then
    printf 'Refusing to replace existing control input: %s\n' "$fifo" >&2
    exit 1
fi
#FIFO 同时保持读写端打开，避免后台 server 因启动先后在 open 阶段阻塞。
mkfifo "$fifo"
exec 3<>"$fifo"

cleanup() {
    #失败或信号路径都停止演示进程、关闭 FIFO 描述符并删除本次创建的 FIFO。
    local status=$?
    trap - INT TERM EXIT
    if [[ -n "$demo_pid" ]] && kill -0 "$demo_pid" 2>/dev/null; then
        kill -INT "$demo_pid" 2>/dev/null || true
        wait "$demo_pid" 2>/dev/null || true
    fi
    exec 3>&-
    rm -f -- "$fifo"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

#在启动后台演示前清空旧控制台日志，避免等待循环命中上一次运行的 worker 输出。
: >"${log_dir}/log_server.console"

#把演示脚本的标准输入接到命名管道，集中控制命令会进入 log_server。
bash "${project_root}/scripts/run_demo.sh" "$build_dir" "$log_dir" "$worker_count" <&3 &
demo_pid=$!

worker_logs_seen=false
for _ in $(seq 1 80); do
    if [[ -f "${log_dir}/log_server.console" ]] &&
       grep -q '\[worker-a\.orders\]' "${log_dir}/log_server.console" &&
       grep -q '\[worker-b\.orders\]' "${log_dir}/log_server.console"; then
        worker_logs_seen=true
        break
    fi
    if ! kill -0 "$demo_pid" 2>/dev/null; then
        wait "$demo_pid" || true
        printf 'Demo ended before workers connected. See %s/log_server.console\n' "$log_dir" >&2
        exit 1
    fi
    sleep 0.1
done
if [[ "$worker_logs_seen" != true ]]; then
    printf 'Timed out waiting for fresh logs from both workers. See %s/log_server.console\n' "$log_dir" >&2
    tail -n 30 "${log_dir}/log_server.console" >&2 || true
    exit 1
fi

#两端实时输出都已出现后再列会话，避免旧日志文件让查询早于 worker 上线。
printf 'list\n' >&3
instances=()
for _ in $(seq 1 40); do
    mapfile -t instances < <(
        grep -E '^control worker ' "${log_dir}/log_server.console" 2>/dev/null |
            sed -nE 's/.*process_instance=([0-9]+).*/\1/p' | sort -u
    )
    if (( ${#instances[@]} >= 2 )); then break; fi
    sleep 0.1
done
if (( ${#instances[@]} < 2 )); then
    printf 'Could not discover both workers. See %s/log_server.console\n' "$log_dir" >&2
    exit 1
fi

first_instance="${instances[0]}"
second_instance="${instances[1]}"
printf 'set-level %s DEBUG\nget-level %s\nset-level %s WARN\nget-level %s\nget-level %s\n' \
    "$first_instance" "$first_instance" "$second_instance" \
    "$second_instance" "$first_instance" >&3

control_confirmed=false
for _ in $(seq 1 40); do
    first_debug_reads="$(grep -Ec "^control get-level process_instance=${first_instance} confirmed=true .*level=DEBUG" \
        "${log_dir}/log_server.console" || true)"
    if grep -q "control set-level process_instance=${first_instance} confirmed=true .*level=DEBUG" \
          "${log_dir}/log_server.console" &&
       grep -q "control set-level process_instance=${second_instance} confirmed=true .*level=WARN" \
          "${log_dir}/log_server.console" &&
       grep -q "control get-level process_instance=${second_instance} confirmed=true .*level=WARN" \
          "${log_dir}/log_server.console" &&
       (( first_debug_reads >= 2 )); then
        control_confirmed=true
        break
    fi
    sleep 0.1
done
if [[ "$control_confirmed" != true ]]; then
    printf 'Central level control was not fully confirmed. See %s/log_server.console\n' "$log_dir" >&2
    tail -n 30 "${log_dir}/log_server.console" >&2 || true
    exit 1
fi

printf 'Central level-control commands are in %s/log_server.console\n' "$log_dir"
wait "$demo_pid"
demo_pid=""
