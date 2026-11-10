#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 /path/to/Light_HLS_Top /path/to/new-output-directory" >&2
  exit 2
fi

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
light_hls_root="$(realpath "${script_directory}/../..")"

python3 "${script_directory}/run_u250_fair_comparison.py" \
  --light-hls-top "$1" \
  --source "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
  --top backend_scalar \
  --testbench "${light_hls_root}/App/backend_scalar/backend_scalar_tb.v" \
  --testbench-top backend_scalar_tb \
  --clock-period-ns 10 \
  --output "$2"
