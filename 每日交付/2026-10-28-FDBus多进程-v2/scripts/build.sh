#!/usr/bin/env bash
#构建独立的示例工程；日志系统作为示例的依赖一起构建，不把示例源码放进产品目标。
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build-demo}"

cmake -S "${project_root}/examples/fdbus-console-demo" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DIPC_LOG_ROOT="${project_root}"
cmake --build "${build_dir}" --target fdbus_console_demo log_server --parallel

printf 'Built:\n  %s\n  %s\n  %s\n' \
    "${build_dir}/fdbus_console_demo" \
    "${build_dir}/ipc_log_project/log_server" \
    "${build_dir}/ipc_log_project/_deps/fdbus-build/name_server"
