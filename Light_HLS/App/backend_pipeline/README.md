# Loop-pipeline backend examples

These applications exercise the supported functional pipeline subset:

- `backend_pipeline.cpp`: independent iterations with achieved II=1;
- `backend_pipeline_memii.cpp`: two reads from one single-port memory, which
  relaxes requested II=1 to achieved II=2;
- `backend_pipeline_recurrence.cpp`: a scalar reduction with exact
  distance-one feedback forwarding;
- `backend_pipeline_memory_dependence.cpp`: an exact loop-carried affine WAR
  dependence whose request-event ordering and single-port memory both require
  achieved II=2;
- `backend_pipeline_inner.cpp`: only the innermost loop is pipelined, with the
  surrounding loop address recurrence preserved as an invariant offset;
- `backend_pipeline_outer.cpp`: the outer loop is pipelined after its inner
  loop is fully unrolled;
- `backend_pipeline_mixed_memory.cpp`: affine read/write ranges are proven
  disjoint, leaving only the single-port memory II constraint;
- `backend_pipeline_nested_conflict.cpp`: an intentionally rejected case with
  PIPELINE on both an outer and inner loop.

The matching Verilog testbenches implement synchronous `ap_memory` models and
check the generated RTL against deterministic source-level results. Each source
also declares a single-port `ram_1p` interface. Light-HLS currently uses the
same single-port behavior by default, while Vitis consumes the explicit
interface pragma during fair-comparison runs.

The testbenches hold `ap_start` until `ap_done` and emit deterministic `RESULT`
traces. Light-HLS and Vitis now expose the same direction-aware native
`ap_memory` signals, including Vitis-style `_r` suffixes for reserved-word
parameter names, so one unmodified testbench connects to both RTL tops.
