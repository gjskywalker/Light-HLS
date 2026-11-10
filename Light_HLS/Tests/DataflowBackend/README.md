# DATAFLOW backend regression

Run the functional DATAFLOW regression with:

```bash
./run_dataflow_backend_tests.sh ../Light_HLS_Top/build/Light_HLS_Top
```

The suite checks:

- scalar streams, stream arrays, FIFO depth, and depth-one backpressure;
- persistent execution with two accepted top transactions in flight;
- task-local PIPELINE and achieved-II reporting;
- cyclic and complete ARRAY_PARTITION on external memories and internal PIPOs;
- combined DATAFLOW, PIPELINE, and ARRAY_PARTITION lowering;
- repeated full-bank PIPOs and exact partial PIPO transfers;
- one-producer/many-consumer FIFO broadcast;
- a cyclic graph made live by an initialized FIFO token;
- top-level scalar `ap_fifo` input/output and external backpressure;
- LLVM IR validity, generated task hierarchy, graph-report contracts, and
  cycle-accurate RTL output with Icarus Verilog;
- generated-FSM task latency, exact all-PIPO latency, exact regular-loop FIFO
  token simulation, and explicit fallback FIFO latency bounds;
- reset-free inferred-RAM FIFO and PIPO payloads, shared partition-bank
  ownership bits, and compact single-terminal controller state.

Negative cases require stable diagnostics for multi-producer fan-in, mismatched
rates, conditional PIPO rates, and zero-token startup cycles. A conditional
FIFO is accepted with an explicit runtime-rate-contract warning and does not
claim a valid static performance estimate. Its test alternates both runtime
paths over 64 reset-free calls, checking zero transfers when disabled and one
transfer when enabled. Both endpoint rate bounds are 0..1.

The separate [FIFOEvents suite](../FIFOEvents/README.md) covers event retention
through frontend optimization, unused reads in pipelined tasks, repeated equal
writes, partial unrolling, and private RAM under natural FIFO backpressure.

U250 is the target used by the regression invocations. Vivado OOC synthesis can
be enabled for an individual case with `--synthesis enable`; the combined
PIPELINE/ARRAY_PARTITION example and the public-FIFO example have both been
verified at 100 MHz on `xcu250-figd2104-2L-e`.

The reset-free FIFO check on Vivado 2025.2 inferred both 4x32 and 8x32 payloads
as LUTRAM. At 100 MHz, `vector_stream_dataflow` used 258 LUT and 369 FF with
8.713 ns WNS, while `vector_stream_pipeline_dataflow` used 464 LUT and 272 FF
with 8.482 ns WNS. Both runs met timing and used no DSP or BRAM tile.
