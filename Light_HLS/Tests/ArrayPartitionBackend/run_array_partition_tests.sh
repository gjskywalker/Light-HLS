#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/Light_HLS_Top" >&2
  exit 2
fi

top_executable="$(realpath "$1")"
if [[ ! -x "${top_executable}" ]]; then
  echo "Light_HLS_Top is not executable: ${top_executable}" >&2
  exit 2
fi

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
light_hls_root="$(realpath "${script_directory}/../..")"
app_directory="${light_hls_root}/App/backend_array_partition"
run_directory="$(mktemp -d /tmp/light-hls-array-partition-test.XXXXXX)"

cleanup()
{
  if [[ $? -ne 0 ]]; then
    echo "Preserving failed test artifacts: ${run_directory}" >&2
    return
  fi
  rm -rf -- "${run_directory}"
}
trap cleanup EXIT

run_case()
{
  local case_name="$1"
  local top_name="backend_array_partition_${case_name}"
  local case_directory="${run_directory}/${case_name}"
  mkdir -p "${case_directory}/work"
  if ! (
    cd "${case_directory}/work"
    "${top_executable}" "${app_directory}/${top_name}.cpp" "${top_name}" \
      --board u250 --freq 100MHz --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1; then
    cat "${case_directory}/light_hls.log" >&2
    exit 1
  fi

  opt -passes=verify \
    "${case_directory}/reports/.lighthls/top_output.bc" -disable-output
  jq empty "${case_directory}/reports/manifest.json" \
    "${case_directory}/reports/binding/binding.json" \
    "${case_directory}/reports/scheduling/pipeline_schedule.json"
  iverilog -g2012 -Wall -s "${top_name}_tb" \
    -o "${case_directory}/rtl_simulation" \
    "${case_directory}/reports/rtl/top.v" \
    "${app_directory}/${top_name}_tb.v" \
    >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/rtl_simulation" \
    >"${case_directory}/simulation.log"
  grep -q 'TESTS PASSED' "${case_directory}/simulation.log"
  tail -1 "${case_directory}/simulation.log"
}

run_case cyclic
jq -e '
  (.memory_objects | any(
    .name == "input" and .partition_kind == "cyclic" and
    .dimensions == [16] and .banks == 4 and .bank_depth == 4 and
    .bank_address_width == 2 and .bank_interface == "ap_memory" and
    (.partition_dimensions == [{"dimension":1, "kind":"cyclic",
      "extent":16, "factor":4, "local_extent":4}]))) and
  ([.memory_access_bindings[] | select(.memory == "input") |
    .possible_banks[0]] | sort == [0,1,2,3])' \
  "${run_directory}/cyclic/reports/binding/binding.json" >/dev/null
jq -e '
  .pipelines[0].requested_ii == 1 and .pipelines[0].achieved_ii == 1 and
  .pipelines[0].memory_mii == 1 and
  .pipelines[0].context_value_count == 0 and
  .pipelines[0].forwarded_load_count == 4 and
  .pipelines[0].derived_value_count == 2 and
  ([.pipelines[0].memory_access_patterns[] | select(.memory == "input") |
    .possible_banks[0]] | sort == [0,1,2,3])' \
  "${run_directory}/cyclic/reports/scheduling/pipeline_schedule.json" \
  >/dev/null

run_case block
jq -e '
  (.memory_objects | any(
    .name == "input" and .partition_kind == "block" and .banks == 4 and
    .bank_depth == 4 and .bank_interface == "ap_memory" and
    .partition_dimensions[0].kind == "block")) and
  ([.memory_access_bindings[] | select(.memory == "input") |
    .possible_banks[0]] | sort == [0,1,2,3])' \
  "${run_directory}/block/reports/binding/binding.json" >/dev/null
jq -e '.pipelines[0].achieved_ii == 1 and .pipelines[0].memory_mii == 1' \
  "${run_directory}/block/reports/scheduling/pipeline_schedule.json" \
  >/dev/null

run_case complete
jq -e '
  (.memory_objects | any(
    .name == "input" and .partition_kind == "complete" and .banks == 4 and
    .bank_depth == 1 and .bank_interface == "scalar" and
    .port_usage[0].rtl_signals == ["scalar_input"])) and
  (.memory_access_bindings | all(
    select(.memory == "input") | .bank_dynamic == false))' \
  "${run_directory}/complete/reports/binding/binding.json" >/dev/null
grep -q 'input wire \[31:0\] input_0' \
  "${run_directory}/complete/reports/rtl/top.v"
if grep -q 'input_0_address' \
  "${run_directory}/complete/reports/rtl/top.v"; then
  echo "Complete partition unexpectedly emitted an ap_memory address port." >&2
  exit 1
fi

run_case pipeline_complete
jq -e '
  (.memory_objects | any(
    .name == "input" and .partition_kind == "complete" and .banks == 4 and
    .bank_depth == 1 and .bank_interface == "scalar")) and
  (.memory_access_bindings | any(
    .memory == "input" and .bank_dynamic == true and
    .possible_banks == [0,1,2,3]))' \
  "${run_directory}/pipeline_complete/reports/binding/binding.json" \
  >/dev/null
jq -e '
  .pipelines[0].requested_ii == 1 and .pipelines[0].achieved_ii == 1 and
  .pipelines[0].memory_mii == 1' \
  "${run_directory}/pipeline_complete/reports/scheduling/pipeline_schedule.json" \
  >/dev/null

run_case complete_rw
jq -e '
  (.memory_objects | any(
    .name == "data" and .partition_kind == "complete" and .banks == 4 and
    .bank_depth == 1 and .bank_interface == "scalar" and
    .access_mode == "read_write" and
    .port_usage[0].rtl_signals == ["scalar_i","scalar_o","ap_vld"] and
    (.bank_port_usage | all(.access_mode == "read_write")))) and
  (.memory_access_bindings | all(
    .memory == "data" and .bank_dynamic == true and
    .possible_banks == [0,1,2,3]))' \
  "${run_directory}/complete_rw/reports/binding/binding.json" >/dev/null
grep -q 'input wire \[31:0\] data_0_i' \
  "${run_directory}/complete_rw/reports/rtl/top.v"
grep -q 'output reg data_3_o_ap_vld' \
  "${run_directory}/complete_rw/reports/rtl/top.v"

run_case multidim
jq -e '
  (.memory_objects | any(
    .name == "input" and .dimensions == [2,4] and .banks == 8 and
    .bank_depth == 1 and .bank_interface == "scalar" and
    (.partition_dimensions | length) == 2 and
    (.partition_dimensions | all(.kind == "complete")))) and
  ([.memory_access_bindings[] | select(.memory == "input") |
    .possible_banks] | sort == [[0,4],[1,5],[2,6],[3,7]])' \
  "${run_directory}/multidim/reports/binding/binding.json" >/dev/null
jq -e '
  .pipelines[0].achieved_ii == 1 and .pipelines[0].memory_mii == 1 and
  (.pipelines[0].memory_access_patterns | all(
    select(.memory == "input") | .bank_mapping_exact == true))' \
  "${run_directory}/multidim/reports/scheduling/pipeline_schedule.json" \
  >/dev/null

run_case multidim_cyclic
jq -e '
  (.memory_objects | any(
    .name == "input" and .dimensions == [2,4] and .banks == 2 and
    .bank_depth == 4 and .bank_address_width == 2 and
    .bank_interface == "ap_memory" and
    .partition_dimensions == [{"dimension":2, "kind":"cyclic",
      "extent":4, "factor":2, "local_extent":2}])) and
  ([.memory_access_bindings[] | select(.memory == "input") |
    .possible_banks[0]] | sort == [0,0,1,1])' \
  "${run_directory}/multidim_cyclic/reports/binding/binding.json" >/dev/null
jq -e '.pipelines[0].achieved_ii == 2 and .pipelines[0].memory_mii == 2' \
  "${run_directory}/multidim_cyclic/reports/scheduling/pipeline_schedule.json" \
  >/dev/null

run_case local
jq -e '
  .direct_local_write_port_count == 4 and
  (.memory_access_bindings | any(.memory == "local" and .is_write and
    .direct_write_data_banks == [0,1,2,3])) and
  (.memory_objects | any(
    .name == "local" and .storage == "local" and
    .partition_kind == "cyclic" and .banks == 4 and .bank_depth == 4 and
    .bank_interface == "local_ram" and
    ([.bank_port_usage[].bank] | unique == [0,1,2,3])))' \
  "${run_directory}/local/reports/binding/binding.json" >/dev/null
grep -q 'local_0_storage' "${run_directory}/local/reports/rtl/top.v"
grep -q 'local_3_storage' "${run_directory}/local/reports/rtl/top.v"
jq -e '
  [.memory_access_bindings[] | select(.memory == "local" and .is_write == false)] |
  length == 4 and all(.bank_mapping_exact and .bank_dynamic == false and
    .bank_analysis_source == "ordinary_loop" and .bank_analysis_iterations == 4) and
  ([.[].possible_banks] | sort == [[0],[1],[2],[3]])' \
  "${run_directory}/local/reports/binding/binding.json" >/dev/null
jq -e '
  .memory_port_conflicts == 0 and
  ([.blocks[].instructions[] | select(.opcode == "load") | .start_cycle] |
    all(. == 0))' \
  "${run_directory}/local/reports/scheduling/schedule.json" >/dev/null

run_expected_failure()
{
  local source_name="$1"
  local top_name="$2"
  local diagnostic="$3"
  local case_directory="${run_directory}/${top_name}"
  mkdir -p "${case_directory}/work"
  if (
    cd "${case_directory}/work"
    "${top_executable}" "${script_directory}/${source_name}" "${top_name}" \
      --board u250 --freq 100MHz --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1; then
    echo "${top_name} unexpectedly succeeded." >&2
    exit 1
  fi
  grep -q "${diagnostic}" "${case_directory}/light_hls.log"
}

run_expected_failure invalid_dimension.cpp invalid_dimension \
  'ARRAY_PARTITION dim=2 is out of range'

echo "Light-HLS array-partition backend tests passed."
