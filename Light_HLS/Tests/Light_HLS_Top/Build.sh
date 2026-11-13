#!/usr/bin/env bash

set -euo pipefail

curr_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cmake -S "${curr_path}" -B "${curr_path}/build" "$@"
# Keep the source driver and the default RL estimator on the same backend build.
cmake --build "${curr_path}/build" --target Light_HLS_Top Light_HLS_DAPO --parallel 4
