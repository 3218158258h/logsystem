#!/usr/bin/env bash
#后台运行现有服务与两个示例，结束后只打印统计结果。
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "${project_root}/scripts/stress_test.py" "$@"
