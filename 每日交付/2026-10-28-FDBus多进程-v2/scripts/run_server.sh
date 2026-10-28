#!/usr/bin/env bash
#前台运行日志系统：后台拉起 name_server，前台保留 log_server 用于显示日志和输入控制命令。
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build-demo}"
log_dir="${2:-${project_root}/logs}"
name_server="${build_dir}/ipc_log_project/_deps/fdbus-build/name_server"
server_bin="${build_dir}/ipc_log_project/log_server"

for executable in "${name_server}" "${server_bin}"; do
    if [[ ! -x "${executable}" ]]; then
        printf 'Missing executable: %s\nRun bash scripts/build.sh first.\n' "${executable}" >&2
        exit 1
    fi
done

mkdir -p "${log_dir}"
name_pid=""
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "${name_pid}" ]]; then
        kill -TERM "${name_pid}" 2>/dev/null || true
        wait "${name_pid}" 2>/dev/null || true
    fi
    exit "${status}"
}
trap cleanup EXIT INT TERM

"${name_server}" >"${log_dir}/name_server.log" 2>&1 &
name_pid=$!
sleep 1
if ! kill -0 "${name_pid}" 2>/dev/null; then
    printf 'name_server failed to start; see %s\n' "${log_dir}/name_server.log" >&2
    exit 1
fi

printf 'name_server pid=%s; log_server runs in the foreground.\n' "${name_pid}"
printf 'control commands: list | get-level <instance> | set-level <instance> <LEVEL>\n'
#log_server 的 stdout 是控制台 Sink，stdin 是控制命令入口；Ctrl-D 只关闭命令输入。
"${server_bin}" --file "${log_dir}/aggregated.log"
