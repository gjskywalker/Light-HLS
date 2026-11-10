#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 /path/to/Light_HLS_Top /path/to/output-directory" >&2
  exit 2
fi

top_executable="$(realpath "$1")"
output_root="$(realpath -m "$2")"
if [[ ! -x "${top_executable}" ]]; then
  echo "Light_HLS_Top is not executable: ${top_executable}" >&2
  exit 2
fi

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
light_hls_root="$(realpath "${script_directory}/../..")"
source_file="${light_hls_root}/App/backend_scalar/backend_scalar.c"

for board in u250 artix7_a12t; do
  mkdir -p "${output_root}/${board}/work"
  (
    cd "${output_root}/${board}/work"
    "${top_executable}" "${source_file}" backend_scalar \
      --board "${board}" --freq 100MHz --synthesis enable \
      --output "${output_root}/${board}"
  ) >"${output_root}/${board}/light_hls.log" 2>&1

  jq -e '.valid == true and
    .qor_kind == "measured_post_rtl_synthesis" and
    .synthesis_mode == "out_of_context" and .vivado_max_threads == 1 and
    (.vivado_cpu_affinity | type == "number")' \
    "${output_root}/${board}/reports/vivado_report/vivado_report.json" \
    >/dev/null
  grep -Fxq 'set_param general.maxThreads 1' \
    "${output_root}/${board}/reports/vivado_report/run_vivado.tcl"
  grep -q -- '-mode out_of_context -flatten_hierarchy rebuilt' \
    "${output_root}/${board}/reports/vivado_report/run_vivado.tcl"
done

jq -n \
  --slurpfile u250 \
    "${output_root}/u250/reports/vivado_report/vivado_report.json" \
  --slurpfile artix7 \
    "${output_root}/artix7_a12t/reports/vivado_report/vivado_report.json" \
  '{
    schema_version: "1.0",
    stage: "cross_board_post_rtl_synthesis",
    primary_board: "u250",
    comparison_rule: "Compare alternatives within one board; do not mix raw cross-board resource counts in one reward.",
    results: {
      u250: $u250[0],
      artix7_a12t: $artix7[0]
    }
  }' >"${output_root}/board_comparison.json"

echo "Board synthesis comparison: ${output_root}/board_comparison.json"
