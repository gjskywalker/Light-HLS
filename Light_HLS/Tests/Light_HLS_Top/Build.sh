#!/usr/bin/env bash

set -euo pipefail

curr_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cmake -S "${curr_path}" -B "${curr_path}/build" "$@"
cmake --build "${curr_path}/build" --parallel 4
