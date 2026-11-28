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
app_directory="${light_hls_root}/App/backend_pipeline"
run_directory="$(mktemp -d /tmp/light-hls-pipeline-test.XXXXXX)"

cleanup()
{
  if [[ $? -ne 0 ]]; then
    echo "Preserving failed test artifacts: ${run_directory}" >&2
    return
  fi
  rm -rf -- "${run_directory}"
}
trap cleanup EXIT

run_light_hls()
{
  local case_name="$1"
  local source_name="$2"
  local top_name="$3"
  local case_directory="${run_directory}/${case_name}"
  mkdir -p "${case_directory}/work"
  if ! (
    cd "${case_directory}/work"
    "${top_executable}" "${app_directory}/${source_name}" "${top_name}" \
      --board u250 --freq 100MHz --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1; then
    cat "${case_directory}/light_hls.log" >&2
    exit 1
  fi
  opt -passes=verify \
    "${case_directory}/reports/.lighthls/top_output.bc" -disable-output
  test -f "${case_directory}/reports/scheduling/pipeline_schedule.rpt"
  test -f "${case_directory}/reports/scheduling/pipeline_schedule.json"
}

run_simulation()
{
  local case_name="$1"
  local testbench_name="$2"
  local testbench_top="$3"
  local dut_define="${4:-}"
  local case_directory="${run_directory}/${case_name}"
  local compile_command=(
    iverilog -g2012 -Wall -s "${testbench_top}"
    -o "${case_directory}/rtl_simulation"
  )
  if [[ -n "${dut_define}" ]]; then
    compile_command+=(-D "DUT_NAME=${dut_define}")
  fi
  compile_command+=(
    "${case_directory}/reports/rtl/top.v"
    "${app_directory}/${testbench_name}"
  )
  "${compile_command[@]}" >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/rtl_simulation" >"${case_directory}/simulation.log"
  grep -q 'TESTS PASSED' "${case_directory}/simulation.log"
  tail -1 "${case_directory}/simulation.log"
}

run_light_hls ii1 backend_pipeline.cpp backend_pipeline
jq -e \
  '.valid == true and (.pipelines | length) == 1 and
   .pipelines[0].requested_ii == 1 and .pipelines[0].achieved_ii == 1 and
   .pipelines[0].trip_count == 16 and
   .pipelines[0].context_value_count == 0 and
   .pipelines[0].forwarded_load_count == 1 and
   .pipelines[0].derived_value_count == 2 and
   .pipelines[0].live_out_value_count == 0' \
  "${run_directory}/ii1/reports/scheduling/pipeline_schedule.json" >/dev/null
jq -e \
  '.state_count == 2 and .latency_cycles == 17 and .hardware_constant_count == 2' \
  "${run_directory}/ii1/reports/scheduling/schedule.json" >/dev/null
jq -e \
  '.register_bits < 1000 and .state_register_bits == 1' \
  "${run_directory}/ii1/reports/binding/post_binding_qor.json" >/dev/null
jq -e \
  '(.memory_objects | any(
     .name == "input" and .port_name == "input_r" and
     .access_mode == "read_only" and
     .port_usage == [{"port":0, "access_mode":"read_only",
       "read":true, "write":false,
       "rtl_signals":["address","ce","q"]}])) and
   (.memory_objects | any(
     .name == "output" and .port_name == "output_r" and
     .access_mode == "write_only" and
     .port_usage == [{"port":0, "access_mode":"write_only",
       "read":false, "write":true,
       "rtl_signals":["address","ce","we","d"]}]))' \
  "${run_directory}/ii1/reports/binding/binding.json" >/dev/null
! grep -q 'input_r_we0\|input_r_d0\|output_r_q0' \
  "${run_directory}/ii1/reports/rtl/top.v"
! grep -q '_active\|_last\|_previous_slot\|_phase' \
  "${run_directory}/ii1/reports/rtl/top.v"
run_simulation ii1 backend_pipeline_tb.v backend_pipeline_tb

run_light_hls memory_ii backend_pipeline_memii.cpp backend_pipeline_memii
jq -e \
  '.valid == true and .pipelines[0].requested_ii == 1 and
   .pipelines[0].achieved_ii == 2 and .pipelines[0].memory_mii == 2 and
   .pipelines[0].limiting_factor == "memory port capacity"' \
  "${run_directory}/memory_ii/reports/scheduling/pipeline_schedule.json" \
  >/dev/null
run_simulation memory_ii backend_pipeline_memii_tb.v \
  backend_pipeline_memii_tb

run_light_hls recurrence backend_pipeline_recurrence.cpp \
  backend_pipeline_recurrence
jq -e \
  '.valid == true and .pipelines[0].achieved_ii == 1 and
   .pipelines[0].recurrence_mii == 1 and
   .pipelines[0].stage_indexed_controller == true and
   .pipelines[0].initialized_at_entry == true and
   .pipelines[0].storage_alias_count == 1 and
   .pipelines[0].value_storage_bits == 32 and
   .pipelines[0].forwarded_load_count == 1 and
   (.pipelines[0].value_storage | all(.slots == 1)) and
   (.pipelines[0].value_storage | any(.retirement_only == true)) and
   .pipelines[0].dependence_analysis_valid == true and
   (.pipelines[0].scalar_recurrences | any(
     .kind == "scalar_recurrence" and .distance == 1 and .exact == true))' \
  "${run_directory}/recurrence/reports/scheduling/pipeline_schedule.json" \
  >/dev/null
run_simulation recurrence backend_pipeline_recurrence_tb.v \
  backend_pipeline_recurrence_tb

run_light_hls memory_dependence backend_pipeline_memory_dependence.cpp \
  backend_pipeline_memory_dependence
jq -e \
  '.valid == true and .pipelines[0].dependence_analysis_valid == true and
   .pipelines[0].memory_dependence_mii == 2 and
   .pipelines[0].achieved_ii == 2 and
   .pipelines[0].conservative_memory_dependence == false and
   .pipelines[0].limiting_factor == "loop-carried memory dependence" and
   (.pipelines[0].memory_dependences | any(
     .kind == "memory_war" and .distance == 1 and
     .source_event == "read_request" and
     .sink_event == "write_request" and .minimum_separation == 1 and
     .loop_carried == true and .exists == true and .exact == true))' \
  "${run_directory}/memory_dependence/reports/scheduling/pipeline_schedule.json" \
  >/dev/null
run_simulation memory_dependence backend_pipeline_memory_dependence_tb.v \
  backend_pipeline_memory_dependence_tb

run_light_hls inner backend_pipeline_inner.cpp backend_pipeline_inner
jq -e \
  '.valid == true and .pipelines[0].loop ==
   "Loop_backend_pipeline_inner_2" and .pipelines[0].trip_count == 16 and
   .pipelines[0].source_trip_count == 4 and
   .pipelines[0].flattened_loop_count == 2 and
   .pipelines[0].achieved_ii == 1 and
   .pipelines[0].conservative_memory_dependence == false and
   (.pipelines[0].memory_access_patterns | all(
     .affine == true and .coefficient == 1 and
     .invariant_term_count == 1)) and
   (.pipelines[0].memory_dependences | all(
     .exists == false and .exact == true))' \
  "${run_directory}/inner/reports/scheduling/pipeline_schedule.json" >/dev/null
run_simulation inner backend_pipeline_nested_tb.v \
  backend_pipeline_nested_tb backend_pipeline_inner

run_light_hls outer backend_pipeline_outer.cpp backend_pipeline_outer
jq -e \
  '.valid == true and .pipelines[0].loop ==
   "Loop_backend_pipeline_outer_1" and .pipelines[0].trip_count == 4 and
   .pipelines[0].memory_mii == 4' \
  "${run_directory}/outer/reports/scheduling/pipeline_schedule.json" >/dev/null
run_simulation outer backend_pipeline_nested_tb.v \
  backend_pipeline_nested_tb backend_pipeline_outer

run_light_hls mixed_memory backend_pipeline_mixed_memory.cpp \
  backend_pipeline_mixed_memory
jq -e \
  '.valid == true and .pipelines[0].achieved_ii == 2 and
   .pipelines[0].memory_mii == 2 and
   .pipelines[0].memory_dependence_mii == 1 and
   .pipelines[0].conservative_memory_dependence == false and
   .pipelines[0].limiting_factor == "memory port capacity" and
   (.pipelines[0].memory_access_patterns | any(
     .mode == "read" and .affine == true and
     .coefficient == 1 and .constant == 0 and
     .minimum == 0 and .maximum == 7)) and
   (.pipelines[0].memory_access_patterns | any(
     .mode == "write" and .affine == true and
     .coefficient == 1 and .constant == 8 and
     .minimum == 8 and .maximum == 15)) and
   (.pipelines[0].memory_dependences | all(
     .exists == false and .exact == true))' \
  "${run_directory}/mixed_memory/reports/scheduling/pipeline_schedule.json" \
  >/dev/null
run_simulation mixed_memory backend_pipeline_mixed_memory_tb.v \
  backend_pipeline_mixed_memory_tb

mkdir -p "${run_directory}/nested_conflict/work"
if (
  cd "${run_directory}/nested_conflict/work"
  "${top_executable}" \
    "${app_directory}/backend_pipeline_nested_conflict.cpp" \
    backend_pipeline_nested_conflict --board u250 --freq 100MHz \
    --output "${run_directory}/nested_conflict"
) >"${run_directory}/nested_conflict/light_hls.log" 2>&1; then
  echo "Nested PIPELINE directives unexpectedly succeeded." >&2
  exit 1
fi
grep -q 'BACKEND_UNSUPPORTED_NESTED_PIPELINES' \
  "${run_directory}/nested_conflict/light_hls.log"

mkdir -p "${run_directory}/row_dependent/work"
(
  cd "${run_directory}/row_dependent/work"
  "${top_executable}" "${script_directory}/row_dependent.cpp" \
    row_dependent --board u250 --freq 100MHz \
    --output "${run_directory}/row_dependent"
) >"${run_directory}/row_dependent/light_hls.log" 2>&1
jq -e '.pipelines[0].flattened_loop_count == 1 and
       .pipelines[0].trip_count == 4' \
  "${run_directory}/row_dependent/reports/scheduling/pipeline_schedule.json" >/dev/null
iverilog -g2012 -Wall -s backend_pipeline_nested_tb \
  -D DUT_NAME=row_dependent -D 'ROW_INCREMENT=(index / 4)' \
  -o "${run_directory}/row_dependent/sim" \
  "${run_directory}/row_dependent/reports/rtl/top.v" \
  "${app_directory}/backend_pipeline_nested_tb.v" \
  >"${run_directory}/row_dependent/iverilog.log" 2>&1
vvp "${run_directory}/row_dependent/sim"

mkdir -p "${run_directory}/triangular_trip"
if "${top_executable}" "${script_directory}/triangular_trip.cpp" triangular_trip \
    --board u250 --freq 100MHz --output "${run_directory}/triangular_trip" \
    >"${run_directory}/triangular_trip/light_hls.log" 2>&1; then
  echo "A maximum trip count was accepted as a fixed pipeline trip count." >&2
  exit 1
fi
grep -q 'BACKEND_PIPELINE_TRIP_COUNT_UNKNOWN' "${run_directory}/triangular_trip/light_hls.log"

for frequency in 100MHz 200MHz; do
  case_directory="${run_directory}/signed_chain_${frequency}"
  mkdir -p "${case_directory}/work"
  (
    cd "${case_directory}/work"
    "${top_executable}" "${script_directory}/signed_chain.cpp" \
      signed_chain --board u250 --freq "${frequency}" --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1
  opt -passes=verify "${case_directory}/reports/.lighthls/top_output.bc" -disable-output
  jq -e '.combinational_value_count > 0 and
         .combinational_delay_budget_ns == (.clock_period_ns * 0.7)' \
    "${case_directory}/reports/scheduling/schedule.json" >/dev/null
  iverilog -g2012 -Wall -s signed_chain_tb -o "${case_directory}/sim" \
    "${case_directory}/reports/rtl/top.v" "${script_directory}/signed_chain_tb.v" \
    >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/sim"
done

echo "Light-HLS pipeline backend tests passed."
