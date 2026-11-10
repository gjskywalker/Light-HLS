# FIFO event preservation regression

```bash
python3 Light_HLS/Tests/FIFOEvents/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-fifo-events
```

The unified `Tests/run_regressions.py` driver also runs this suite. Artifacts
are retained. The local `hls_stream.h` is a synthesis-only frontend shim, not
a software queue. Its hash is recorded separately because changing this header
affects source compilation without changing the compiler executable hash.

Six positive configurations run 64 calls without reset each (384 calls and
6,144 output comparisons), then repeat with injected consumer stalls:

- Ordinary tasks with FIFO depth 1, depth 3, and depth 4.
- Pipelined tasks with four reads/writes per iteration, including an unused
  read result, two equal constant writes, `read(T&)`, and stream operators.
- Partially unrolled tasks (`factor=2`) with the same event sequence.
- The unchanged historical private-buffer reproducer from
  `LocalWriteData/known_failures/private_stream_buffer.cpp`. Despite its retained
  historical path, it is now a required passing test, not an expected failure.

Every major IR checkpoint must verify and retain at least the original static
volatile event count. Exact dynamic graph rates and RTL handshake counts must
agree: 64 tokens for the four-event fixtures, and 32/16 tokens on the private
buffer's two channels. The latter requires natural empty/full FIFO stalls,
exactly 32 committed local RAM writes, paused local-write coverage, and unchanged
RAM contents at every stalled edge. The normal tests use natural backpressure.
A separate simulation top additionally forces the active consumer's clock
enable low for four out of every thirteen cycles. It never stalls an idle task,
preserving the wrapper's start-pulse contract. Both runs use the same event and
output oracle; the injected run is not a cross-tool performance measurement.

Structural checks distinguish request-edge capture for ordinary internal FIFO
consumers from the retained registered response of pipeline consumers. The
existing initialized-cycle test requires registered responses on cyclic graphs.

The output oracle is independent integer arithmetic in the Verilog testbench.
Deterministic random input words and scalar arguments change between calls;
the scalar input also changes immediately after acceptance in local tests.
Generic volatile RAM remains unsupported, both at the top and inside a task
that also has a valid FIFO endpoint. Negative cases must return a normal error
with `BACKEND_UNSUPPORTED_MEMORY_OPERATION` and must not emit RTL.

`FIFO_INTERNAL_MONITOR` enables Light-HLS hierarchy checks. For cross-tool
comparison, omit that define and enable `FAIR_COMPARISON`: scalar arguments
remain stable until completion, and the test emits per-transaction cycle counts
and a common `RESULT` trace. `PRIVATE_BUFFER` selects the historical fixture's
independent arithmetic oracle and input depth.

The existing DATAFLOW suite additionally tests 64 alternating enabled/disabled
transactions through conditional stream reads and writes. Both endpoint rate
bounds are correctly reported as 0..1; no unconditional token transfer is
allowed on the disabled path. Static performance remains explicitly unknown.

These are bounded simulation tests, not a formal all-input equivalence proof.
