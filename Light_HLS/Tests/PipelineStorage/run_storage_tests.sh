#!/usr/bin/env bash
set -euo pipefail

top_executable="$(realpath "$1")"
script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
run_directory="$(mktemp -d /tmp/light-hls-pipeline-storage.XXXXXX)"
cleanup() {
  if [[ $? -ne 0 ]]; then
    echo "Preserving failed test artifacts: ${run_directory}" >&2
    return
  fi
  rm -rf -- "${run_directory}"
}
trap cleanup EXIT

for latency in 1 2; do
  case_directory="${run_directory}/latency${latency}"
  mkdir -p "${case_directory}"
  extra_arguments=()
  if [[ "${latency}" == 2 ]]; then
    extra_arguments+=("${script_directory}/latency2.cfg")
  fi
  (
    cd "${case_directory}"
    "${top_executable}" "${script_directory}/storage_stress.cpp" storage_stress \
      "${extra_arguments[@]}" --board u250 --freq 100MHz --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1
  opt -passes=verify "${case_directory}/reports/.lighthls/top_output.bc" -disable-output
  jq -e '.pipelines[0].value_storage_bits < .pipelines[0].uncompacted_value_storage_bits and
         (.pipelines[0].value_storage | any(.slots == 1))' \
    "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
  if [[ "${latency}" == 1 ]]; then
    jq -e '.pipelines[0].forwarded_load_count == 1 and
           (.pipelines[0].value_storage | any(.slots > 1))' \
      "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
  else
    jq -e '.pipelines[0].forwarded_load_count == 0' \
      "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
  fi
  iverilog -g2012 -Wall -D "READ_LATENCY=${latency}" -s storage_stress_tb \
    -o "${case_directory}/sim" "${case_directory}/reports/rtl/top.v" \
    "${script_directory}/storage_stress_tb.v" >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/sim"
done

for latency in 1 2; do
  case_directory="${run_directory}/dynamic_bank_latency${latency}"
  mkdir -p "${case_directory}"
  extra_arguments=()
  if [[ "${latency}" == 2 ]]; then
    extra_arguments+=("${script_directory}/latency2.cfg")
  fi
  (
    cd "${case_directory}"
    "${top_executable}" "${script_directory}/dynamic_bank_storage.cpp" dynamic_bank_storage \
      "${extra_arguments[@]}" --board u250 --freq 100MHz --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1
  jq -e --argjson latency "${latency}" '.pipelines[0].forwarded_load_count == (if $latency == 1 then 1 else 0 end) and
         .pipelines[0].value_storage_bits < .pipelines[0].uncompacted_value_storage_bits' \
    "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
  iverilog -g2012 -Wall -D BANKED_INPUT -D DUT_NAME=dynamic_bank_storage \
    -D "READ_LATENCY=${latency}" -s storage_stress_tb -o "${case_directory}/sim" \
    "${case_directory}/reports/rtl/top.v" "${script_directory}/storage_stress_tb.v" \
    >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/sim"
done

case_directory="${run_directory}/two_slot"
mkdir -p "${case_directory}"
(
  cd "${case_directory}"
  "${top_executable}" "${script_directory}/two_slot_storage.cpp" two_slot_storage \
    --board u250 --freq 100MHz --output "${case_directory}"
) >"${case_directory}/light_hls.log" 2>&1
jq -e '.pipelines[0].context_slots == 4 and .pipelines[0].achieved_ii == 2 and
       (.pipelines[0].value_storage | any(.slots == 2))' \
  "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
iverilog -g2012 -Wall -D TWO_READS -D DUT_NAME=two_slot_storage -s storage_stress_tb \
  -o "${case_directory}/sim" "${case_directory}/reports/rtl/top.v" \
  "${script_directory}/storage_stress_tb.v" >"${case_directory}/iverilog.log" 2>&1
vvp "${case_directory}/sim"

case_directory="${run_directory}/dual_port"
mkdir -p "${case_directory}"
(
  cd "${case_directory}"
  "${top_executable}" "${script_directory}/dual_port_forward.cpp" dual_port_forward \
    "${script_directory}/dual_port.cfg" \
    --board u250 --freq 100MHz --output "${case_directory}"
) >"${case_directory}/light_hls.log" 2>&1
jq -e '.pipelines[0].forwarded_load_count == 2 and
       .pipelines[0].achieved_ii == 1 and .pipelines[0].stage_indexed_controller == true' \
  "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
iverilog -g2012 -Wall -D DUAL_INPUT -D DUT_NAME=dual_port_forward -s storage_stress_tb \
  -o "${case_directory}/sim" "${case_directory}/reports/rtl/top.v" \
  "${script_directory}/storage_stress_tb.v" >"${case_directory}/iverilog.log" 2>&1
vvp "${case_directory}/sim"

case_directory="${run_directory}/backpressure"
mkdir -p "${case_directory}"
(
  cd "${case_directory}"
  "${top_executable}" "${script_directory}/backpressured_pipeline.cc" backpressured_pipeline \
    --board u250 --freq 100MHz --output "${case_directory}"
) >"${case_directory}/light_hls.log" 2>&1
iverilog -g2012 -Wall -s backpressure_tb \
  -o "${case_directory}/sim" "${case_directory}/reports/rtl/top.v" \
  "${script_directory}/backpressure_tb.v" \
  >"${case_directory}/iverilog.log" 2>&1
vvp "${case_directory}/sim"
echo "Light-HLS pipeline storage tests passed."

for mode in runtime constant memory; do
  case_directory="${run_directory}/boundary_${mode}"
  mkdir -p "${case_directory}"
  (
    cd "${case_directory}"
    "${top_executable}" "${script_directory}/boundary_${mode}_seed.cpp" "boundary_${mode}_seed" \
      --board u250 --freq 100MHz --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1
  extra_arguments=()
  if [[ "${mode}" == constant ]]; then
    extra_arguments+=(-D CONSTANT_SEED)
    expected_initialization=true
  else
    expected_initialization=true
    if [[ "${mode}" == memory ]]; then
      extra_arguments+=(-D MEMORY_SEED)
      expected_initialization=false
    fi
  fi
  jq -e --argjson initialized "${expected_initialization}" \
    '.pipelines[0].initialized_at_entry == $initialized and
     .pipelines[0].storage_alias_count == 1' \
    "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
  iverilog -g2012 -Wall -D "DUT_NAME=boundary_${mode}_seed" "${extra_arguments[@]}" \
    -s boundary_recurrence_tb -o "${case_directory}/sim" \
    "${case_directory}/reports/rtl/top.v" "${script_directory}/boundary_recurrence_tb.v" \
    >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/sim"
done
echo "Light-HLS pipeline boundary tests passed."

case_directory="${run_directory}/adjacent_recurrence"
mkdir -p "${case_directory}"
(
  cd "${case_directory}"
  "${top_executable}" "${script_directory}/adjacent_recurrence.cpp" adjacent_recurrence \
    --board u250 --freq 100MHz --output "${case_directory}"
) >"${case_directory}/light_hls.log" 2>&1
jq -e '(.pipelines | length) == 2 and .pipelines[0].initialized_at_entry and
       (.pipelines[1].initialized_at_entry | not)' \
  "${case_directory}/reports/scheduling/pipeline_schedule.json" >/dev/null
iverilog -g2012 -Wall -D DUT_NAME=adjacent_recurrence -D ADJACENT_SEED \
  -s boundary_recurrence_tb -o "${case_directory}/sim" \
  "${case_directory}/reports/rtl/top.v" "${script_directory}/boundary_recurrence_tb.v" \
  >"${case_directory}/iverilog.log" 2>&1
vvp "${case_directory}/sim"
echo "Light-HLS adjacent pipeline tests passed."
