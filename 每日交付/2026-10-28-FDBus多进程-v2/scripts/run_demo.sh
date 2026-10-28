#!/usr/bin/env bash
#在独立终端运行一个示例客户端；持续读 stdin，每条输入调用一次日志库。
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
name="${1:-demo-a}"
shift || true
build_dir="${DEMO_BUILD_DIR:-${project_root}/build-demo}"
demo_bin="${build_dir}/fdbus_console_demo"

if [[ ! -x "${demo_bin}" ]]; then
    printf 'Missing executable: %s\nRun bash scripts/build.sh first.\n' "${demo_bin}" >&2
    exit 1
fi

exec "${demo_bin}" --name "${name}" "$@"
