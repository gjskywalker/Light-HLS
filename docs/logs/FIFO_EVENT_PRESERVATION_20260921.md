# FIFO event preservation checkpoint

This implements the correctness prerequisite identified in the
[September 18 local-memory checkpoint](LOCAL_MEMORY_PAYLOAD_OPTIMIZATION_20260918.md).
It does not implement ordinary induction/update register sharing or claim a
general performance improvement over Vitis.

## Failure and representation

The local synthesis header previously represented a stream as ordinary scalar
storage. In the unchanged private-buffer reproducer, initial Clang `-O1`
optimization moved 32 intended reads out of a loop and reduced 16 writes to
one final store. No backend scheduling algorithm can reconstruct those lost
events reliably.

The synthesis-only header now uses volatile event storage, without a default
initialization write. It is not a C simulation queue. Volatility survives all
frontend stages and is never stripped to make validation pass:

- `HI_RemoveRedundantAccess` cannot merge/forward non-simple accesses.
- `HI_InstructionMoveBackward` cannot hoist side-effecting instructions.
- `HI_VarWidthReduce` preserves volatility, alignment, atomic ordering, and
  synchronization scope when rebuilding a store.
- DATAFLOW task configuration explicitly identifies internal and public FIFO
  argument indices. Stream-array endpoint discovery follows integer address
  PHIs and casts instead of relying on readonly/writeonly argument attributes.
- MemoryObject analysis validates volatile-access provenance. Generic volatile
  RAM and atomics remain unsupported; having another valid FIFO argument does
  not exempt an unrelated volatile memory access.
- Ordinary schedules serialize FIFO requests in source order, including
  read-after-read. Pipeline dependence analysis adds `fifo_order` request-edge
  constraints within and across iterations, independently of RAM alias rules.
  Stream-array endpoint ordering currently remains conservative.
- FIFO reads are pipeline hardware-liveness roots even if their return values
  are unused. Post-RTL validation requires exactly one scheduled port binding
  for every FIFO event. Missing token operations cannot silently become a
  supposedly valid schedule.

Only the frontend shim and compiler passes changed; none of the positive
kernel algorithms was rewritten to avoid the failing behavior.

## Analysis stability

An additional run reproduced a ScalarEvolution crash from DATAFLOW access-rate
analysis on the existing nested pipeline/partition fixture. The rate pass now
owns a coherent per-function set of dominators, loop information, assumptions,
target-library information, and ScalarEvolution, rather than borrowing legacy
analysis references around nested task pass managers. The snapshot lives until
all channel rates have been queried and no IR is mutated during its lifetime.

The final executable compiles that nested fixture successfully in 20 separate
processes. Its RTL is byte-identical to the existing measured baseline. This
is stability evidence for the changed access-rate path, not proof that every
legacy assertion or LLVM issue has been eliminated. The previous exact
analysis-corruption mechanism was not established and is not claimed here.

## Validation

- All 13 unified local regression suites pass.
- All 18 independent App RTL smoke tests pass.
- New [FIFOEvents tests](../../Light_HLS/Tests/FIFOEvents/README.md): five positive
  configurations, 320 calls without reset, 5,120 output comparisons, and two
  explicit generic-volatile rejection cases.
- Each positive fixture verifies six IR checkpoints, exact dynamic channel
  rates, exact FIFO handshake counts, and independent arithmetic outputs.
  Partial unroll doubles static event count from 8 to 16 while retaining
  exactly 64 dynamic tokens. Four-event pipeline tasks both achieve II=4,
  consistent with a single FIFO port.
- The historical private-buffer fixture now transfers exactly 32 `loaded`
  tokens and 16 `computed` tokens per call. Depth-one FIFO full/empty stalls
  occur naturally; private RAM commits exactly 32 writes and remains unchanged
  during stalled edges, including paused write requests.
- The existing conditional FIFO test now alternates enabled and disabled paths
  across 64 calls. Disabled transactions transfer no tokens. Both endpoints
  have rate bounds 0..1; static performance remains explicitly unknown.

The existing 20-design measured RTL set is unchanged relative to the latest
applicable baselines: 19 files match September 14 byte-for-byte; the local-array
file matches September 18's payload checkpoint byte-for-byte. The September 14
audit deliberately reports one mismatch for that already-updated local case;
a separate September 18 audit passes. No old measurements were rewritten or
presented as fresh synthesis results. In particular, the existing local App's
43 cycles / 297 LUT / 181 FF are retained measurements, not a new QoR gain.

## Fresh U250 comparison

The previously failing private-buffer source now passes the same independent
testbench for Light-HLS and Vitis HLS 2025.2: all 1,024 output words from 64
reset-free calls match. Vivado 2025.2 uses the same normalized-wrapper OOC
boundary, U250 `xcu250-figd2104-2L-e`, and a 10 ns constraint.

| Metric | Light-HLS | Vitis HLS |
| --- | ---: | ---: |
| Observed cycles per transaction | 760 | 389 |
| LUT | 354 | 324 |
| FF | 522 | 316 |
| DSP / BRAM tiles | 0 / 0 | 0 / 0 |
| Synthesis WNS (ns) | +8.738 | +8.923 |

This is a new correctness/QoR baseline, not a before/after speedup: the old
implementation could not reliably compile this FIFO source. Light-HLS still
needs roughly 1.95 times the cycles and has a substantial register gap here.
Both meet the synthesis clock constraint; neither result is post-route timing.

The common bench holds scalar arguments stable and holds `ap_start` until an
`ap_ready` rising-edge acknowledgment. It models Vitis's extra native memory
ports and unused directions, while synthesis uses normalized wrappers. Harmless
repeated reads of ordinary input RAM are permitted equally for both tools;
all output addresses, values, and write counts remain exact. Separate local
tests retain exact FIFO-event counts and change scalars after acceptance. No
FIFO status signal is forced in this comparison. See the
[portable evidence archive](../baselines/u250_100mhz_fifo_events_20260921/README.md).

## Reproduction

```bash
taskset -c 0 cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-regressions --keep-going
python3 Light_HLS/Tests/FairComparison/run_u250_fair_comparison.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --source Light_HLS/Tests/LocalWriteData/known_failures/private_stream_buffer.cpp \
  --top local_dataflow \
  --testbench Light_HLS/Tests/FIFOEvents/events_tb.v \
  --testbench-top events_tb \
  --testbench-define DUT_NAME=local_dataflow \
  --testbench-define PRIVATE_BUFFER \
  --testbench-define FAIR_COMPARISON \
  --vitis-testbench-define VITIS_MEMORY_PORTS \
  --clock-period-ns 10 --output /tmp/fresh-private-fifo-comparison
```

Raw artifacts and the frozen final executable are retained under
`/tmp/lighthls-fifo-events-20260921.sXuNPi/`. Final executable SHA256:
`8c03681db4a57cfa4ed67ddd3df5cfaea04564a4b32c115897918fa915053eeb`.
The frontend header is a runtime dependency with its own SHA256:
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.

## Remaining work

The next QoR target remains ordinary induction/update register duplication and
its operand muxes, with memory-edge liveness checks before sharing registers.
The new FIFO regression must stay enabled while making those changes. More
aggressive loop scheduling and DATAFLOW task balancing should use measured
cycle traces, not the pre-backend estimate as a substitute for generated RTL.
Child-local partition lookup and unsupported consecutive pipeline-loop shapes
from the prior checkpoint are not expanded here. These tests do not establish
formal all-input equivalence, post-route timing closure, or universal Vitis
parity; RL and a scheduler replacement remain later stages.
