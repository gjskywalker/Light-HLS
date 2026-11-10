#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/Light_HLS_Top" >&2
  exit 2
fi

top_executable="$(realpath "$1")"
script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
light_hls_root="$(realpath "${script_directory}/../..")"
app_directory="${light_hls_root}/App/backend_storage_binding"
run_directory="$(mktemp -d /tmp/light-hls-storage-binding-test.XXXXXX)"

cleanup()
{
  rm -rf -- "${run_directory}"
}
trap cleanup EXIT

mkdir -p "${run_directory}/work"
(
  cd "${run_directory}/work"
  "${top_executable}" \
    "${app_directory}/backend_storage_binding.cpp" \
    backend_storage_binding --board u250 --freq 100MHz \
    --output "${run_directory}"
) >"${run_directory}/light_hls.log" 2>&1

opt -passes=verify \
  "${run_directory}/reports/.lighthls/top_output.bc" -disable-output
jq -e '
  (.memory_objects | any(
    .name == "bram_buffer" and .storage == "local" and
    .binding_type == "ram_1p" and .implementation == "bram" and
    .storage_binding_explicit == true and .ports == 1 and
    .read_latency == 2)) and
  (.memory_objects | any(
    .name == "uram_buffer" and .storage == "local" and
    .binding_type == "ram_2p" and .implementation == "uram" and
    .storage_binding_explicit == true and .ports == 2 and
    .read_latency == 2)) and
  (.memory_objects | any(
    .name == "lutram_buffer" and .storage == "local" and
    .binding_type == "ram_1p" and .implementation == "lutram" and
    .storage_binding_explicit == true and .ports == 1 and
    .read_latency == 1))' \
  "${run_directory}/reports/binding/binding.json" >/dev/null

grep -q '(\* ram_style = "block" \*) reg' \
  "${run_directory}/reports/rtl/top.v"
grep -q '(\* ram_style = "ultra" \*) reg' \
  "${run_directory}/reports/rtl/top.v"
grep -q '(\* ram_style = "distributed" \*) reg' \
  "${run_directory}/reports/rtl/top.v"

iverilog -g2012 -Wall -s backend_storage_binding_tb \
  -o "${run_directory}/rtl_simulation" \
  "${run_directory}/reports/rtl/top.v" \
  "${app_directory}/backend_storage_binding_tb.v" \
  >"${run_directory}/iverilog.log" 2>&1
vvp "${run_directory}/rtl_simulation" \
  >"${run_directory}/simulation.log"
grep -q 'TESTS PASSED' "${run_directory}/simulation.log"
tail -1 "${run_directory}/simulation.log"
echo "Light-HLS storage-binding backend tests passed."
