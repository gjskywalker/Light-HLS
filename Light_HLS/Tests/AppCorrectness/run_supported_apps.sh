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
run_directory="$(mktemp -d /tmp/light-hls-app-correctness.XXXXXX)"

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
  local source_file="$2"
  local top_function="$3"
  local reference_file="$4"
  local testbench_file="$5"
  local testbench_top="$6"

  local case_directory="${run_directory}/${case_name}"
  local backend_directory="${case_directory}/reports"
  mkdir -p "${case_directory}/work"

  if ! (
    cd "${case_directory}/work"
    "${top_executable}" "${source_file}" "${top_function}" \
      --board u250 --freq 100MHz --synthesis disable \
      --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1; then
    cat "${case_directory}/light_hls.log" >&2
    exit 1
  fi

  clang++ -std=c++17 -x c++ "${source_file}" -x c++ "${reference_file}" \
    -o "${case_directory}/source_reference"
  "${case_directory}/source_reference" >"${case_directory}/source.trace"

  opt -passes=verify \
    "${backend_directory}/.lighthls/top_output.bc" -disable-output

  test -f "${backend_directory}/report.rpt"
  test -f "${backend_directory}/scheduling/schedule.rpt"
  test -f "${backend_directory}/scheduling/pipeline_schedule.json"
  test -f "${backend_directory}/scheduling/pipeline_schedule.rpt"
  test -f "${backend_directory}/binding/binding.rpt"
  test -f "${backend_directory}/rtl/rtl_generation.rpt"
  test -f "${backend_directory}/vivado_report/vivado_report.rpt"
  test -f "${backend_directory}/summary/qor_summary.rpt"
  grep -q '^== Light-HLS Report Index' "${backend_directory}/report.rpt"

  iverilog -g2012 -Wall -s "${testbench_top}" \
    -o "${case_directory}/rtl_simulation" \
    "${backend_directory}/rtl/top.v" "${testbench_file}"
  vvp "${case_directory}/rtl_simulation" >"${case_directory}/rtl.log"
  grep '^RESULT ' "${case_directory}/rtl.log" \
    >"${case_directory}/rtl.trace"
  cmp "${case_directory}/source.trace" "${case_directory}/rtl.trace"
  tail -1 "${case_directory}/rtl.log"
}

array_app="${light_hls_root}/App/2dloop1darray_notpl"
run_case \
  pragma_free_array \
  "${array_app}/2dloop1darray_notpl.c" \
  f \
  "${array_app}/2dloop1darray_notpl_ref.cpp" \
  "${array_app}/2dloop1darray_notpl_tb.v" \
  pragma_free_array_tb

jq -e \
  '.valid == true and .pragma_configuration.pipeline == {} and
   .pragma_configuration.array_partition_count == 0 and
   .pragma_configuration.dataflow_count == 0 and
   .memory_objects == [{"name":"A", "protocol":"ap_memory",
     "storage":"external", "binding_type":"auto",
     "implementation":"auto", "storage_binding_explicit":false,
     "port_name":"A", "element_width":32,
     "address_width":6, "depth":58, "read_latency":1,
     "combinational_scalar_reads":false,
     "fifo_read_on_request":false,
     "depth_source":"inferred", "depth_exact":false,
     "inferred_minimum_depth":58, "partition_kind":"none",
     "dimensions":[58], "partition_dimensions":[], "banks":1,
     "bank_depth":58, "bank_address_width":6,
     "bank_interface":"ap_memory", "ports":1,
     "access_mode":"read_write",
     "port_usage":[{"port":0, "access_mode":"read_write",
       "read":true, "write":true,
       "rtl_signals":["address","ce","we","d","q"]}],
     "bank_port_usage":[{"bank":0, "port":0,
       "access_mode":"read_write", "read":true, "write":true}]}]' \
  "${run_directory}/pragma_free_array/reports/binding/binding.json" >/dev/null

spam_app="${light_hls_root}/App/spam-filter"
run_case \
  spam_filter \
  "${spam_app}/sgd.cpp" \
  compute \
  "${spam_app}/sgd_ref.cpp" \
  "${spam_app}/sgd_tb.v" \
  spam_filter_tb

jq -e \
  '.valid == true and .pragma_configuration.pipeline == {} and
   .pragma_configuration.array_partition_count == 0 and
   .pragma_configuration.dataflow_count == 0 and
   (.memory_objects | length) == 3 and
   (.memory_objects | all(.protocol == "ap_memory" and
                          .storage == "external" and
                          .binding_type == "auto" and
                          .implementation == "auto" and
                          .storage_binding_explicit == false and
                          .element_width == 32 and .address_width == 10 and
                          .depth == 800 and .read_latency == 1 and
                          .depth_source == "frontend" and
                          .depth_exact == true and
                          .inferred_minimum_depth == 800 and
                          .banks == 1 and .ports == 1)) and
   (.memory_objects | map(.name) | sort) ==
     ["gradient", "theta_local", "training_instance"]' \
  "${run_directory}/spam_filter/reports/binding/binding.json" >/dev/null

echo "Supported pragma-free App correctness tests passed."
