#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/Light_HLS_Top" >&2
  exit 2
fi

top_executable="$(realpath "$1")"
script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
light_hls_root="$(realpath "${script_directory}/../..")"
app_directory="${light_hls_root}/App/light_hls_dataflow"
run_directory="$(mktemp -d /tmp/light-hls-dataflow-test.XXXXXX)"

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
  local source_name="$1"
  local top_name="$2"
  local testbench_name="$3"
  local testbench_top="$4"
  local compiler_define="${5:-}"
  local case_directory="${run_directory}/${6:-${top_name}}"

  mkdir -p "${case_directory}/work"
  (
    cd "${case_directory}/work"
    "${top_executable}" \
      "${app_directory}/${source_name}" \
      "${top_name}" --board u250 --freq 100MHz \
      --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1

  opt -passes=verify \
    "${case_directory}/reports/.lighthls/top_output.bc" -disable-output
  grep -q 'module light_hls_fifo' \
    "${case_directory}/reports/rtl/top.v"
  grep -q "module ${top_name}" \
    "${case_directory}/reports/rtl/top.v"

  local compile_arguments=(-g2012 -Wall -s "${testbench_top}")
  if [[ -n "${compiler_define}" ]]; then
    compile_arguments+=("-D${compiler_define}")
  fi
  iverilog "${compile_arguments[@]}" \
    -o "${case_directory}/rtl_simulation" \
    "${case_directory}/reports/rtl/top.v" \
    "${app_directory}/${testbench_name}" \
    >"${case_directory}/iverilog.log" 2>&1
  vvp "${case_directory}/rtl_simulation" \
    >"${case_directory}/simulation.log"
  grep -q 'TESTS PASSED' "${case_directory}/simulation.log"
  tail -1 "${case_directory}/simulation.log"
}

run_case vector_stream_dataflow.cc vector_stream_dataflow \
  vector_stream_dataflow_tb.v vector_stream_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  .rates_balanced == true and .deadlock_free == true and
  .cyclic == false and .graph_ii > 0 and
  .first_transaction_latency_exact == true and
  .first_transaction_latency_lower_bound == 130 and
  .first_transaction_latency_upper_bound == 130 and
  .first_transaction_latency_source ==
    "generated task FSM access cycles plus bounded FIFO token simulation and retirement" and
  (.topological_order | length) == 3 and (.tasks | length) == 3 and
  (.channels | length) == 2 and
  (.channels | any(.name == "loaded" and .type == "fifo" and
                   .width == 32 and .depth == 4 and .instances == 1 and
                   .rate_known == true and .production_rate == 64 and
                   .consumption_rate == 64 and .minimum_depth == 1 and
                   .producer_access_timing_exact == true and
                   .producer_first_access_cycle == 2 and
                   .producer_access_interval == 2 and
                   .consumer_access_timing_exact == true and
                   .consumer_first_access_cycle == 1 and
                   .consumer_access_interval == 1 and
                   .buffer_count == 1 and .capacity_safe == true)) and
  (.channels | any(.name == "computed" and .type == "fifo" and
                   .width == 32 and .depth == 8 and .instances == 1 and
                   .rate_known == true and .production_rate == 64 and
                   .consumption_rate == 64 and .minimum_depth == 1 and
                   .producer_first_access_cycle == 1 and
                   .producer_access_interval == 1 and
                   .consumer_first_access_cycle == 1 and
                   .consumer_access_interval == 1 and
                   .buffer_count == 1 and .capacity_safe == true))' \
  "${run_directory}/vector_stream_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -q 'generate_reset_free_payload' \
  "${run_directory}/vector_stream_dataflow/reports/rtl/top.v"
grep -q 'ram_style = "auto"' \
  "${run_directory}/vector_stream_dataflow/reports/rtl/top.v"

run_case vector_stream_dataflow_depth1.cc vector_stream_dataflow_depth1 \
  vector_stream_dataflow_tb.v vector_stream_dataflow_tb \
  DATAFLOW_DUT=vector_stream_dataflow_depth1
jq -e '
  .valid == true and .performance_valid == true and
  (.channels | length) == 2 and
  (.channels | all(.depth == 1 and .instances == 1 and
                   .production_rate == 64 and .consumption_rate == 64 and
                   .capacity_safe == true))' \
  "${run_directory}/vector_stream_dataflow_depth1/reports/scheduling/dataflow_graph.json" \
  >/dev/null

run_case vector_array_dataflow.cc vector_array_dataflow \
  vector_array_dataflow_tb.v vector_array_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  .rates_balanced == true and .deadlock_free == true and
  .graph_ii == 129 and
  .first_transaction_latency_exact == true and
  .first_transaction_latency_lower_bound == 386 and
  .first_transaction_latency_upper_bound == 386 and
  (.tasks | all(.latency_exact == true and
                .latency_source ==
                  "generated RTL FSM with static loop trips")) and
  (.tasks | length) == 3 and
  (.channels | length) == 2 and
  (.channels | all(.type == "pipo" and .width == 32 and
                   .depth == 64 and .instances == 1 and
                   .production_rate == 64 and .consumption_rate == 64 and
                   .minimum_depth == 64 and .buffer_count == 2 and
                   .capacity_safe == true))' \
  "${run_directory}/vector_array_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -q 'module light_hls_pipo' \
  "${run_directory}/vector_array_dataflow/reports/rtl/top.v"
grep -q 'ram_style = "auto"' \
  "${run_directory}/vector_array_dataflow/reports/rtl/top.v"
grep -q '.PEEK_ENABLED(0)' \
  "${run_directory}/vector_array_dataflow/reports/rtl/top.v"
if grep -q '_completion_credit' \
  "${run_directory}/vector_array_dataflow/reports/rtl/top.v"; then
  echo "A single-terminal graph must not allocate completion credits." >&2
  exit 1
fi
if grep -q 'channel_loaded_0_instance' \
  "${run_directory}/vector_array_dataflow/reports/rtl/top.v"; then
  echo "Local-array DATAFLOW channels must not use the FIFO implementation." >&2
  exit 1
fi

run_case vector_stream_array_dataflow.cc vector_stream_array_dataflow \
  vector_stream_array_dataflow_tb.v vector_stream_array_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  (.tasks | length) == 2 and
  (.channels | length) == 1 and
  (.channels[0].type == "fifo") and
  (.channels[0].width == 32) and (.channels[0].depth == 4) and
  (.channels[0].instances == 6) and
  (.channels[0].production_rate == 6) and
  (.channels[0].consumption_rate == 6) and
  (.channels[0].minimum_depth == 1) and
  (.channels[0].buffer_count == 1) and
  (.channels[0].capacity_safe == true)' \
  "${run_directory}/vector_stream_array_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null

run_case vector_stream_persistent_dataflow.cc \
  vector_stream_persistent_dataflow \
  vector_stream_persistent_dataflow_tb.v \
  vector_stream_persistent_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  .execution_model == "persistent bounded task queue" and
  .maximum_outstanding_transactions == 2 and
  .minimum_acceptance_interval == 1 and
  .measured_acceptance_interval == null' \
  "${run_directory}/vector_stream_persistent_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -q 'reg compute_stage_1_read_bank;' \
  "${run_directory}/vector_stream_persistent_dataflow/reports/rtl/top.v"
if grep -Eq 'reg (load_stage_0|store_stage_2)_read_bank;' \
  "${run_directory}/vector_stream_persistent_dataflow/reports/rtl/top.v"; then
  echo "Tasks without scalar arguments must not allocate scalar read-bank state." >&2
  exit 1
fi

run_case vector_stream_pipeline_dataflow.cc vector_stream_pipeline_dataflow \
  vector_stream_dataflow_tb.v vector_stream_dataflow_tb \
  DATAFLOW_DUT=vector_stream_pipeline_dataflow
jq -e '
  .valid == true and .performance_valid == true and
  .first_transaction_latency_exact == true and
  .first_transaction_latency_lower_bound == 69 and
  .first_transaction_latency_upper_bound == 69 and
  (.tasks | length) == 3 and
  (.tasks | all(.pipeline_count == 1 and
                .pipeline_initiation_interval != null)) and
  (.channels | all(.producer_access_timing_exact == true and
                   .consumer_access_timing_exact == true and
                   .producer_access_interval == 1 and
                   .consumer_access_interval == 1))' \
  "${run_directory}/vector_stream_pipeline_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null

run_case vector_external_partition_dataflow.cc \
  vector_external_partition_dataflow \
  vector_external_partition_dataflow_tb.v \
  vector_external_partition_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  (.channels | length) == 1 and
  (.channels[0].type == "fifo")' \
  "${run_directory}/vector_external_partition_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -q 'input_r_3_address0' \
  "${run_directory}/vector_external_partition_dataflow/reports/rtl/top.v"

run_case vector_array_partition_dataflow.cc \
  vector_array_partition_dataflow \
  vector_array_partition_dataflow_tb.v \
  vector_array_partition_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  (.channels | length) == 1 and
  (.channels[0].type == "pipo") and
  (.channels[0].partition == "cyclic") and
  (.channels[0].banks == 4) and (.channels[0].bank_depth == 16)' \
  "${run_directory}/vector_array_partition_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -q 'channel_stage_bank_3_instance' \
  "${run_directory}/vector_array_partition_dataflow/reports/rtl/top.v"

run_case vector_complete_partition_dataflow.cc \
  vector_complete_partition_dataflow \
  vector_complete_partition_dataflow_tb.v \
  vector_complete_partition_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  (.channels[0].partition == "complete") and
  (.channels[0].banks == 8) and (.channels[0].bank_depth == 1)' \
  "${run_directory}/vector_complete_partition_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -q '.PEEK_ENABLED(1)' \
  "${run_directory}/vector_complete_partition_dataflow/reports/rtl/top.v"

run_case vector_pipeline_partition_dataflow.cc \
  vector_pipeline_partition_dataflow \
  vector_pipeline_partition_dataflow_tb.v \
  vector_pipeline_partition_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  .graph_ii == 18 and
  .first_transaction_latency_exact == true and
  .first_transaction_latency_lower_bound == 35 and
  .first_transaction_latency_upper_bound == 35 and
  (.tasks | all(.pipeline_count == 1 and
                .pipeline_initiation_interval == 1)) and
  (.channels[0].partition == "cyclic") and
  (.channels[0].banks == 4)' \
  "${run_directory}/vector_pipeline_partition_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null
grep -Fq 'output wire [31:0] stage_0_d0' \
  "${run_directory}/vector_pipeline_partition_dataflow/reports/rtl/top.v"
grep -Fq 'assign stage_0_d0 = ' \
  "${run_directory}/vector_pipeline_partition_dataflow/reports/rtl/top.v"
if [[ $(grep -c 'reg channel_stage_ready_' \
  "${run_directory}/vector_pipeline_partition_dataflow/reports/rtl/top.v") \
  -ne 2 ]]; then
  echo "Partitioned PIPO banks must share one pair of ready bits." >&2
  exit 1
fi

run_case vector_partial_pipo_dataflow.cc vector_partial_pipo_dataflow \
  vector_partial_pipo_dataflow_tb.v vector_partial_pipo_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  (.channels[0].type == "pipo") and
  (.channels[0].depth == 4) and
  (.channels[0].production_rate == 3) and
  (.channels[0].consumption_rate == 3) and
  (.channels[0].capacity_safe == true)' \
  "${run_directory}/vector_partial_pipo_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null

run_case vector_broadcast_dataflow.cc vector_broadcast_dataflow \
  vector_broadcast_dataflow_tb.v vector_broadcast_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  (.channels | length) == 2 and
  (.channels | all(.broadcast == true and .type == "fifo" and
                   .production_rate == 16 and .consumption_rate == 16))' \
  "${run_directory}/vector_broadcast_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null

run_case initialized_cycle_dataflow.cc initialized_cycle_dataflow \
  initialized_cycle_dataflow_tb.v initialized_cycle_dataflow_tb
jq -e '
  .valid == true and .performance_valid == true and
  .cyclic == true and .zero_token_cycle == false and
  .deadlock_free == true and
  (.channels | any(.initial_tokens == 1)) and
  (.channels | all(.consumer_read_on_request == false))' \
  "${run_directory}/initialized_cycle_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null

run_case public_fifo_dataflow.cc public_fifo_dataflow \
  public_fifo_dataflow_tb.v public_fifo_dataflow_tb
grep -q 'input_stream_empty_n' \
  "${run_directory}/public_fifo_dataflow/reports/rtl/top.v"
grep -q 'output_stream_full_n' \
  "${run_directory}/public_fifo_dataflow/reports/rtl/top.v"

run_case conditional_fifo_dataflow.cc conditional_fifo_dataflow \
  conditional_fifo_dataflow_tb.v conditional_fifo_dataflow_tb
jq -e '
  .valid == true and .performance_valid == false and
  .rates_balanced == false and
  (.channels | length) == 1 and
  (.channels[0].type == "fifo") and
  (.channels[0].rate_known == false) and
  (.channels[0].rate_bounds_known == true) and
  (.channels[0].runtime_rate_check_required == true) and
  (.channels[0].minimum_production_rate == 0) and
  (.channels[0].maximum_production_rate == 1) and
  (.channels[0].minimum_consumption_rate == 0) and
  (.channels[0].maximum_consumption_rate == 1)' \
  "${run_directory}/conditional_fifo_dataflow/reports/scheduling/dataflow_graph.json" \
  >/dev/null

invalid_directory="${run_directory}/invalid_multiple_producers"
mkdir -p "${invalid_directory}/work"
set +e
(
  cd "${invalid_directory}/work"
  "${top_executable}" \
    "${script_directory}/invalid_multiple_producers.cc" \
    invalid_multiple_producers --board u250 --freq 100MHz \
    --output "${invalid_directory}"
) >"${invalid_directory}/light_hls.log" 2>&1
invalid_status=$?
set -e
if [[ ${invalid_status} -eq 0 ]]; then
  echo "Expected the multiple-producer DATAFLOW graph to be rejected." >&2
  exit 1
fi
grep -q 'BACKEND_DATAFLOW_MULTIPLE_PRODUCERS' \
  "${invalid_directory}/reports/rtl/failure.json"

run_invalid_case()
{
  local source_name="$1"
  local top_name="$2"
  local diagnostic_code="$3"
  local case_directory="${run_directory}/${top_name}"

  mkdir -p "${case_directory}/work"
  set +e
  (
    cd "${case_directory}/work"
    "${top_executable}" \
      "${script_directory}/${source_name}" \
      "${top_name}" --board u250 --freq 100MHz \
      --output "${case_directory}"
  ) >"${case_directory}/light_hls.log" 2>&1
  local status=$?
  set -e
  if [[ ${status} -eq 0 ]]; then
    echo "Expected ${top_name} to be rejected." >&2
    exit 1
  fi
  grep -q "${diagnostic_code}" \
    "${case_directory}/reports/rtl/failure.json"
}

run_invalid_case invalid_rate_mismatch.cc invalid_rate_mismatch \
  BACKEND_DATAFLOW_RATE_MISMATCH
run_invalid_case invalid_conditional_pipo.cc invalid_conditional_pipo \
  BACKEND_DATAFLOW_PIPO_RATE_UNKNOWN
run_invalid_case invalid_zero_token_cycle.cc invalid_zero_token_cycle \
  BACKEND_DATAFLOW_ZERO_TOKEN_CYCLE

run_case ../../Tests/DataflowBackend/nested_pipeline_partition_dataflow.cc \
  vector_pipeline_partition_dataflow vector_pipeline_partition_dataflow_tb.v \
  vector_pipeline_partition_dataflow_tb "" nested_pipeline_partition
nested_directory="${run_directory}/nested_pipeline_partition"
jq -e '.first_transaction_latency_exact == true and
       .first_transaction_latency_lower_bound < 60 and
       .first_transaction_latency_lower_bound == .first_transaction_latency_upper_bound and
       (.tasks | all(.latency_exact == true and .pipeline_count == 1))' \
  "${nested_directory}/reports/scheduling/dataflow_graph.json" >/dev/null
reported_cycles="$(jq '.first_transaction_latency_lower_bound + 1' \
  "${nested_directory}/reports/scheduling/dataflow_graph.json")"
observed_cycles="$(rg -o 'cycles=[0-9]+' "${nested_directory}/simulation.log" \
  | tail -n 1 | cut -d= -f2)"
test "${reported_cycles}" -eq "${observed_cycles}"

echo "Light-HLS DATAFLOW backend tests passed."
