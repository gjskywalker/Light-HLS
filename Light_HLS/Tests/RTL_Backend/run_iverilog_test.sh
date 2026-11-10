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
run_directory="$(mktemp -d /tmp/light-hls-rtl-test.XXXXXX)"

cleanup()
{
  if [[ $? -ne 0 ]]; then
    echo "Preserving failed test artifacts: ${run_directory}" >&2
    return
  fi
  rm -rf -- "${run_directory}"
}
trap cleanup EXIT

mkdir -p "${run_directory}/scalar" "${run_directory}/memory" \
  "${run_directory}/depth_inferred" "${run_directory}/depth_config" \
  "${run_directory}/depth_invalid" "${run_directory}/local_memory" \
  "${run_directory}/single_port" "${run_directory}/dual_port"

if "${top_executable}" \
  "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
  backend_scalar --freq 100 --synthesis disable \
  >"${run_directory}/invalid_frequency.log" 2>&1; then
  echo "A unitless --freq value unexpectedly succeeded." >&2
  exit 1
fi
grep -q 'frequency must use a positive value with a unit' \
  "${run_directory}/invalid_frequency.log"

if "${top_executable}" \
  "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
  backend_scalar --fre 100MHz --synthesis disable \
  >"${run_directory}/invalid_frequency_option.log" 2>&1; then
  echo "The removed --fre option unexpectedly succeeded." >&2
  exit 1
fi
grep -q -- '--fre is not a valid option; use --freq' \
  "${run_directory}/invalid_frequency_option.log"

if "${top_executable}" \
  "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
  backend_scalar --synthesis disable \
  >"${run_directory}/missing_frequency.log" 2>&1; then
  echo "A run without --freq unexpectedly succeeded." >&2
  exit 1
fi
grep -q -- 'Missing required option: --freq <frequency>' \
  "${run_directory}/missing_frequency.log"

if "${top_executable}" \
  "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
  backend_scalar --freq 100MHz --synthesis yes \
  >"${run_directory}/invalid_synthesis.log" 2>&1; then
  echo "An invalid --synthesis value unexpectedly succeeded." >&2
  exit 1
fi
grep -q -- '--synthesis requires enable or disable' \
  "${run_directory}/invalid_synthesis.log"

if ! (
  cd "${run_directory}/scalar"
  HI_CLOCK_PERIOD=8 "${top_executable}" \
    "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
    backend_scalar --freq 100MHz
) >"${run_directory}/scalar.log" 2>&1; then
  cat "${run_directory}/scalar.log" >&2
  exit 1
fi

clang++ -std=c++17 \
  -x c++ \
  "${light_hls_root}/App/backend_scalar/backend_scalar.c" \
  "${light_hls_root}/App/backend_scalar/backend_scalar_ref.c" \
  -o "${run_directory}/backend_scalar_source"
"${run_directory}/backend_scalar_source" \
  >"${run_directory}/backend_scalar_source.trace"

clang++ "${run_directory}/scalar/reports/.lighthls/top_output.bc" -x c++ \
  "${light_hls_root}/App/backend_scalar/backend_scalar_ref.c" \
  -o "${run_directory}/backend_scalar_ir"
"${run_directory}/backend_scalar_ir" \
  >"${run_directory}/backend_scalar_ir.trace"
cmp "${run_directory}/backend_scalar_source.trace" \
  "${run_directory}/backend_scalar_ir.trace"

iverilog -g2012 -Wall -s backend_scalar_tb \
  -o "${run_directory}/backend_scalar_sim" \
  "${run_directory}/scalar/reports/rtl/top.v" \
  "${light_hls_root}/App/backend_scalar/backend_scalar_tb.v"
vvp "${run_directory}/backend_scalar_sim" \
  >"${run_directory}/backend_scalar_rtl.log"
cat "${run_directory}/backend_scalar_rtl.log"
grep '^RESULT ' "${run_directory}/backend_scalar_rtl.log" \
  >"${run_directory}/backend_scalar_rtl.trace"
cmp "${run_directory}/backend_scalar_source.trace" \
  "${run_directory}/backend_scalar_rtl.trace"

jq -e '.valid == true and .stage == "manifest" and
  .clock_period_ns == 10 and .target_frequency_mhz == 100 and
  .artifacts.human_report_index == "report.rpt" and
  .artifacts.intermediate_directory == ".lighthls" and
  .artifacts.schedule_report == "scheduling/schedule.rpt" and
  .artifacts.pipeline_schedule == "scheduling/pipeline_schedule.json" and
  .artifacts.pipeline_schedule_report ==
    "scheduling/pipeline_schedule.rpt" and
  .artifacts.binding_report == "binding/binding.rpt"' \
  "${run_directory}/scalar/reports/manifest.json" >/dev/null
jq -e '.valid == true and .qor_kind == "structural_post_binding"' \
  "${run_directory}/scalar/reports/binding/post_binding_qor.json" >/dev/null
jq -e '.requested == false and .status == "not_requested"' \
  "${run_directory}/scalar/reports/vivado_report/vivado_report.json" >/dev/null
test -f "${run_directory}/scalar/reports/report.rpt"
test -f "${run_directory}/scalar/reports/scheduling/schedule.rpt"
test -f "${run_directory}/scalar/reports/scheduling/pipeline_schedule.json"
test -f "${run_directory}/scalar/reports/scheduling/pipeline_schedule.rpt"
test -f "${run_directory}/scalar/reports/binding/binding.rpt"
test -f "${run_directory}/scalar/reports/rtl/rtl_generation.rpt"
test -f "${run_directory}/scalar/reports/vivado_report/vivado_report.rpt"
test -f "${run_directory}/scalar/reports/summary/qor_summary.rpt"
grep -q '^== Light-HLS Report Index' \
  "${run_directory}/scalar/reports/report.rpt"
test -f "${run_directory}/scalar/reports/.lighthls/tmp_loopLabeled.cc"
test -f "${run_directory}/scalar/reports/.lighthls/top_output.ll"
grep -Fxq 'clock = 10' \
  "${run_directory}/scalar/reports/.lighthls/effective_config.cfg"
test ! -e "${run_directory}/scalar/tmp_loopLabeled.cc"
test ! -e "${run_directory}/scalar/top_output.bc"

if ! (
  cd "${run_directory}/memory"
  "${top_executable}" \
    "${light_hls_root}/App/extremeLoop/extremeLoop.cpp" \
    extremeLoop --freq 100MHz --output "${run_directory}/memory_output"
) >"${run_directory}/memory.log" 2>&1; then
  cat "${run_directory}/memory.log" >&2
  exit 1
fi

iverilog -g2012 -Wall -s extremeLoop_tb \
  -o "${run_directory}/extremeLoop_sim" \
  "${run_directory}/memory_output/reports/rtl/top.v" \
  "${light_hls_root}/App/extremeLoop/extremeLoop_tb.v"
vvp "${run_directory}/extremeLoop_sim"

jq -e \
  '.valid == true and .memory_object_count == 2 and
   .memory_port_count == 2 and .memory_access_count == 4' \
  "${run_directory}/memory_output/reports/binding/post_binding_qor.json" >/dev/null
jq -e \
  '(.memory_objects | length) == 2 and
   (.memory_access_bindings | length) == 4 and
   (.memory_objects | all(.protocol == "ap_memory" and
                          .read_latency == 1 and .ports == 1 and
                          .depth_source == "frontend" and
                          .depth_exact == true and
                          .address_width == 3)) and
   (.memory_objects | map(.depth) | sort) == [6, 8]' \
  "${run_directory}/memory_output/reports/binding/binding.json" >/dev/null

if ! (
  cd "${run_directory}/depth_inferred"
  "${top_executable}" \
    "${light_hls_root}/App/backend_memory_depth/backend_memory_depth.cpp" \
    backend_memory_depth --freq 100MHz \
    --output "${run_directory}/depth_inferred_output"
) >"${run_directory}/depth_inferred.log" 2>&1; then
  cat "${run_directory}/depth_inferred.log" >&2
  exit 1
fi

jq -e \
  '.memory_objects == [{"name":"memory", "protocol":"ap_memory",
    "storage":"external",
    "binding_type":"auto", "implementation":"auto",
    "storage_binding_explicit":false,
    "port_name":"memory", "element_width":32, "address_width":2,
    "depth":4, "read_latency":1, "depth_source":"inferred",
    "combinational_scalar_reads":false,
    "fifo_read_on_request":false,
    "depth_exact":false, "inferred_minimum_depth":4,
    "partition_kind":"none", "dimensions":[4],
    "partition_dimensions":[], "banks":1, "bank_depth":4,
    "bank_address_width":2, "bank_interface":"ap_memory",
    "ports":1, "access_mode":"read_write",
    "port_usage":[{"port":0, "access_mode":"read_write",
      "read":true, "write":true,
      "rtl_signals":["address","ce","we","d","q"]}],
    "bank_port_usage":[{"bank":0, "port":0,
      "access_mode":"read_write", "read":true, "write":true}]}]' \
  "${run_directory}/depth_inferred_output/reports/binding/binding.json" >/dev/null

if ! (
  cd "${run_directory}/depth_config"
  "${top_executable}" \
    "${light_hls_root}/App/backend_memory_depth/backend_memory_depth.cpp" \
    backend_memory_depth \
    "${light_hls_root}/App/backend_memory_depth/backend_memory_depth.cfg" \
    --freq 100MHz \
    --output "${run_directory}/depth_config_output"
) >"${run_directory}/depth_config.log" 2>&1; then
  cat "${run_directory}/depth_config.log" >&2
  exit 1
fi

iverilog -g2012 -Wall -s backend_memory_depth_tb \
  -o "${run_directory}/backend_memory_depth_sim" \
  "${run_directory}/depth_config_output/reports/rtl/top.v" \
  "${light_hls_root}/App/backend_memory_depth/backend_memory_depth_tb.v"
vvp "${run_directory}/backend_memory_depth_sim"

jq -e \
  '.memory_depth_configuration.memory == 16 and
   .memory_objects[0].depth == 16 and
   .memory_objects[0].address_width == 4 and
   .memory_objects[0].depth_source == "configuration" and
   .memory_objects[0].depth_exact == true and
   .memory_objects[0].inferred_minimum_depth == 4' \
  "${run_directory}/depth_config_output/reports/binding/binding.json" >/dev/null

if (
  cd "${run_directory}/depth_invalid"
  "${top_executable}" \
    "${light_hls_root}/App/backend_memory_depth/backend_memory_depth.cpp" \
    backend_memory_depth \
    "${light_hls_root}/App/backend_memory_depth/backend_memory_depth_too_small.cfg" \
    --freq 100MHz \
    --output "${run_directory}/depth_invalid_output"
) >"${run_directory}/depth_invalid.log" 2>&1; then
  cat "${run_directory}/depth_invalid.log" >&2
  echo "The undersized memory depth test unexpectedly succeeded." >&2
  exit 1
fi

jq -e \
  '.valid == false and
   (.errors | any(.code == "BACKEND_MEMORY_DEPTH_TOO_SMALL"))' \
  "${run_directory}/depth_invalid_output/reports/rtl/failure.json" >/dev/null
grep -q 'BACKEND_MEMORY_DEPTH_TOO_SMALL' \
  "${run_directory}/depth_invalid_output/reports/rtl/rtl_generation.rpt"

if ! (
  cd "${run_directory}/local_memory"
  "${top_executable}" \
    "${light_hls_root}/App/backend_local_memory/backend_local_memory.cpp" \
    backend_local_memory \
    "${light_hls_root}/App/backend_local_memory/backend_local_memory.cfg" \
    --board u250 --freq 100MHz \
    --output "${run_directory}/local_memory_output"
) >"${run_directory}/local_memory.log" 2>&1; then
  cat "${run_directory}/local_memory.log" >&2
  exit 1
fi

iverilog -g2012 -Wall -s backend_local_memory_tb \
  -o "${run_directory}/backend_local_memory_sim" \
  "${run_directory}/local_memory_output/reports/rtl/top.v" \
  "${light_hls_root}/App/backend_local_memory/backend_local_memory_tb.v"
vvp "${run_directory}/backend_local_memory_sim"

jq -e \
  '.memory_objects == [{"name":"local", "protocol":"internal_ram",
    "storage":"local", "binding_type":"auto",
    "implementation":"auto", "storage_binding_explicit":false,
    "port_name":"local", "element_width":32,
    "address_width":3, "depth":8, "read_latency":2,
    "combinational_scalar_reads":false,
    "fifo_read_on_request":false,
    "depth_source":"allocation", "depth_exact":true,
    "inferred_minimum_depth":null, "partition_kind":"none",
    "dimensions":[8], "partition_dimensions":[], "banks":1,
    "bank_depth":8, "bank_address_width":3,
    "bank_interface":"local_ram", "ports":2,
    "access_mode":"read_write",
    "port_usage":[
      {"port":0, "access_mode":"read_write", "read":true,
       "write":true, "rtl_signals":["address","ce","we","d","q"]},
      {"port":1, "access_mode":"read_only", "read":true,
       "write":false, "rtl_signals":["address","ce","we","d","q"]}],
    "bank_port_usage":[{"bank":0, "port":0,
      "access_mode":"read_write", "read":true, "write":true},
      {"bank":0, "port":1, "access_mode":"read_only",
       "read":true, "write":false}]}] and
   .memory_interface_configuration.local == {"ports":2, "read_latency":2}' \
  "${run_directory}/local_memory_output/reports/binding/binding.json" >/dev/null

if ! (
  cd "${run_directory}/single_port"
  "${top_executable}" \
    "${light_hls_root}/App/backend_dual_memory/backend_dual_memory.cpp" \
    backend_dual_memory --board u250 --freq 100MHz \
    --output "${run_directory}/single_port_output"
) >"${run_directory}/single_port.log" 2>&1; then
  cat "${run_directory}/single_port.log" >&2
  exit 1
fi

jq -e \
  '.memory_port_conflicts == 1 and
   .memory_port_serialization_cycles == 1 and
   ([.blocks[].instructions[] | select(.opcode == "load") |
     .start_cycle] | sort) == [0, 1]' \
  "${run_directory}/single_port_output/reports/scheduling/schedule.json" >/dev/null
jq -e \
  '.memory_objects[0].storage == "external" and
   .memory_objects[0].ports == 1 and .memory_port_count == 1 and
   ([.memory_access_bindings[].port] | unique) == [0]' \
  "${run_directory}/single_port_output/reports/binding/binding.json" >/dev/null

if ! (
  cd "${run_directory}/dual_port"
  "${top_executable}" \
    "${light_hls_root}/App/backend_dual_memory/backend_dual_memory.cpp" \
    backend_dual_memory \
    "${light_hls_root}/App/backend_dual_memory/backend_dual_memory.cfg" \
    --board u250 --freq 100MHz \
    --output "${run_directory}/dual_port_output"
) >"${run_directory}/dual_port.log" 2>&1; then
  cat "${run_directory}/dual_port.log" >&2
  exit 1
fi

iverilog -g2012 -Wall -s backend_dual_memory_tb \
  -o "${run_directory}/backend_dual_memory_sim" \
  "${run_directory}/dual_port_output/reports/rtl/top.v" \
  "${light_hls_root}/App/backend_dual_memory/backend_dual_memory_tb.v"
vvp "${run_directory}/backend_dual_memory_sim"

jq -e \
  '.memory_port_conflicts == 0 and
   .memory_port_serialization_cycles == 0 and
   ([.blocks[].instructions[] | select(.opcode == "load") |
     .start_cycle] | unique) == [0]' \
  "${run_directory}/dual_port_output/reports/scheduling/schedule.json" >/dev/null
jq -e \
  '.memory_objects[0].storage == "external" and
   .memory_objects[0].ports == 2 and .memory_port_count == 2 and
   .memory_objects[0].access_mode == "read_only" and
   (.memory_objects[0].port_usage | all(
      .access_mode == "read_only" and .read == true and
      .write == false and .rtl_signals == ["address","ce","q"])) and
   ([.memory_access_bindings[].port] | sort) == [0, 1]' \
  "${run_directory}/dual_port_output/reports/binding/binding.json" >/dev/null

mkdir -p "${run_directory}/memory_order/work"
(
  cd "${run_directory}/memory_order/work"
  "${top_executable}" "${script_directory}/memory_request_order.cpp" \
    memory_request_order --board u250 --freq 100MHz \
    --output "${run_directory}/memory_order"
) >"${run_directory}/memory_order/light_hls.log" 2>&1
opt -passes=verify "${run_directory}/memory_order/reports/.lighthls/top_output.bc" \
  -disable-output
jq -e '.memory_ordering_cycles > 0' \
  "${run_directory}/memory_order/reports/scheduling/schedule.json" >/dev/null
iverilog -g2012 -Wall -s memory_request_order_tb \
  -o "${run_directory}/memory_order/sim" \
  "${run_directory}/memory_order/reports/rtl/top.v" \
  "${script_directory}/memory_request_order_tb.v"
vvp "${run_directory}/memory_order/sim"

echo "Light-HLS RTL backend tests passed."
