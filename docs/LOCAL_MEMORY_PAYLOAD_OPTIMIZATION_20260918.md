# Local-memory payload binding optimization

This checkpoint continues the
[ordinary-bank optimization](ORDINARY_BANK_OPTIMIZATION_20260918.md).
It removes avoidable local RAM write-data gating without changing schedules,
memory latencies, bank/port reservations, or external interface behavior.

## Measured bottleneck

In the unchanged local-array App, the previous Light-HLS and Vitis netlists
both used 128 LUTs as distributed RAM and 128 FFs for local RAM read outputs.
The remaining gap was not caused by selecting a larger RAM implementation:
Light-HLS had 300 logic LUTs versus Vitis's 114. Generated RTL gated the same
32-bit write payload independently by FSM state and bank selection for each
of four banks, even though each bank/port had only one static writer.

## Implementation and safety contract

- `HI_ResourceBinding` counts store instructions per local memory bank/port
  after normal port assignment. Exactly one static writer enables a direct
  payload connection for that bank/port. Multiple writers keep the existing
  mux; two writers on separate physical ports can qualify independently.
- `HI_RTLGeneration` emits a continuous wire for qualifying payloads, using
  the same ordinary or pipeline-stage value expression as before. Expressions
  are rendered after final interface names are resolved, including reserved
  names such as `input` becoming `input_r`.
- Address selection, CE, WE, pipeline-valid conditions, and task `ap_ce` are
  unchanged. Data may change while writing is disabled; it is sampled only
  at an enabled write edge. There is no combinational local RAM read path.
- External `ap_memory` and complete-partition scalar ports are unchanged.
  An internal optimization does not relax their inactive-cycle data behavior.
- Post-RTL validation independently verifies the sole-writer, local-storage,
  valid-bank, and unique-bank conditions for every direct binding.

Binding JSON/RPT adds `direct_local_write_port_count`, per-access
`direct_write_data_banks`, and a Direct Local Write Data table. These describe
structural decisions, not predicted LUT savings. Parent DATAFLOW reports do
not yet export child binding tables; child RTL still uses the same binder.

## Validation

The final compiler passes all 12 local regression suites and the independent
18-case App RTL smoke test. The new
[LocalWriteData suite](../Light_HLS/Tests/LocalWriteData/README.md) checks eight
fixtures, 512 calls without reset, and 8,192 outputs. It covers ordinary and
pipelined writers, dynamic bank selection, one/two-cycle local memory reads,
multiple writers, two physical ports, partially eligible banks, and unchanged
external writes. Scalar ports change immediately after start acceptance.

The PIPO DATAFLOW fixture has a private RAM and pipeline writer. Controlled
stalls are injected at the wrapper's shared task clock-enable, gating both
requests and the child datapath. Every call must include paused local-write
cycles, commit exactly 32 writes, and preserve every local RAM word during
each paused edge. This is a stall-injection test, not natural FIFO congestion.

The eight positive fixtures do not include the known failing private-buffer
FIFO combination described below. No formal all-input equivalence is claimed.

## U250 same-flow comparison

The same unmodified `backend_array_partition_local` App and common self-checking
testbench were compiled using Vitis/Vivado 2025.2, U250
`xcu250-figd2104-2L-e`, and a 10 ns clock constraint. Final numbers use the
common normalized-wrapper Vivado OOC synthesis boundary.

| Metric | Before this change | After this change | Vitis |
| --- | ---: | ---: | ---: |
| Observed cycles | 43 | 43 | 42 |
| LUT | 428 | 297 | 242 |
| LUT as logic | 300 | 169 | 114 |
| LUT as distributed RAM | 128 | 128 | 128 |
| FF | 184 | 181 | 152 |
| Local RAM read-output FF | 128 | 128 | 128 |

LUT decreases by 30.6%, with unchanged cycles. Synthesis WNS is +8.681 ns
for Light-HLS and +9.003 ns for Vitis. DSP and BRAM-tile counts remain zero.
The strict no-cycle/resource-regressions comparison passes. The preliminary
native-boundary synthesis reported 318 LUT; it is not substituted for the
final normalized-wrapper result of 297 LUT.

The synthesized FF breakdown explains the remaining target: Light-HLS has
48 ordinary induction/update FFs and five FSM/status FFs besides the 128 RAM
output FFs. Vitis has 24 non-RAM-output FFs in total. Reducing induction/update
duplication and its operand muxes is more concrete than replacing the complete
scheduler. It still requires a proof that old induction values remain available
through their last memory-use edge; blindly aliasing PHIs with early update
registers would be incorrect. FF reduction is not yet implemented here.

All 20 historical baseline sources were regenerated. Nineteen RTL files remain
byte-identical to the measured September 14 archive and therefore also to the
September 18 ordinary-bank checkpoint. Only the local-array RTL changes.
Those nineteen designs were not resynthesized or assigned new measurement
fingerprints. A fresh identity audit proves the final compiler regenerates the
newly measured local RTL exactly. See the
[measurement archive](baselines/u250_100mhz_payload_20260918/README.md).

Cycle fields retain the existing testbench completion convention. These are
post-synthesis results, not post-route timing closure or universal Vitis parity.

## Newly isolated FIFO limitation

A stronger test exposed an existing front-end correctness limitation, preserved
as `Tests/LocalWriteData/known_failures/private_stream_buffer.cpp`. The current
local `hls::stream` stub implements stream events as ordinary non-volatile
loads/stores. In that source's initial `top.bc`, before the backend runs,
32 intended reads have already become one preheader load and 16 intended
writes have become one final store. This is observable event loss, not a depth
estimate or write-data mux issue.

The previous compiler rejects that source with production/consumption rate
mismatches. Some development runs additionally abort in ScalarEvolution from
`HI_DataflowPerformanceAnalysis::accessRate`. GDB confirms the sampled function
and ScalarEvolution owner agree; an analysis-owner mismatch was not established.
The assertion's root cause remains unresolved and is not claimed fixed.
The new stack-trace registration successfully captured its caller chain.

No stream stub, optimization pipeline, or rate diagnostic was weakened to make
the test pass. The positive private-memory DATAFLOW test uses PIPO arrays.
The FIFO reproducer and failure evidence are separate from successful results.
The earlier child-local ARRAY_PARTITION lookup and consecutive pipeline-loop
shape limitations also remain outside this optimization's supported expansion.

## Reproduction and next work

Build with `cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2`, then run
`Tests/run_regressions.py` with the compiler path and a fresh `--output` directory.
The existing `Tests/FairComparison/run_u250_fair_comparison.py` command in the
ordinary-bank notes reproduces the same local App comparison. The new
`Tests/FairComparison/dump_vivado_cells.tcl` dumps primitive names from a
synthesized checkpoint for register attribution.

Raw artifacts and before/final compiler snapshots are retained under
`/tmp/lighthls-local-mux-qor-20260918.4T7n35/`. Final compiler SHA256:
`24266e01ef47fcde15715edbc5777b9841dc38f92b79be45c1e59b4a3810091d`.

The next correctness priority is an explicit, optimization-safe representation
of FIFO reads/writes before ordinary LLVM optimization, followed by event-count,
ordering, backpressure, and repeated-transaction regressions. Merely making
storage volatile requires auditing the legacy custom passes and backend
validation too; it is not a complete fix by itself. The next QoR priority is
ordinary induction/update storage and mux reduction with memory-edge liveness
proofs. Neither requires jumping directly to SDC or RL.
