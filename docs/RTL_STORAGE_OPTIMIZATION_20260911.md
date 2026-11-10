# Response forwarding and lifetime-aware RTL storage

This checkpoint continues the clock-aware backend work described in
`QOR_OPTIMIZATION_20260908.md`. It changes generated hardware, not source
pragmas, and does not replace the scheduler with an SDC solver.

## Implementation contracts

- `HI_OperationScheduling` tentatively forwards a one-cycle memory response
  when its bank is statically selected. It follows combinational expressions
  to every actual sampling edge. All uses must occur on the response-ready
  edge; otherwise explicit capture is restored and the block is rescheduled.
  This monotonically decreasing candidate set is used for both ordinary
  basic blocks and modulo-scheduled pipeline regions. Multicycle responses,
  dynamic bank selection, cross-block uses, and recurrence-producing loads
  keep the conservative capture path.
- Ordinary blocks may perform their final side effect and control transition
  on the same edge. Branch conditions, return values, and incoming values of
  successor PHIs must already be ready. A successor that reads an ordinary
  registered result does so on the next edge; its PHI assignments are treated
  separately because they sample on the transition edge itself.
- `HI_PipelineStorageAnalysis` is an independent pass between scheduling and
  binding, including inside DATAFLOW child backends. Stored values are given
  a write edge and last sampling edge, following zero-cycle expressions and
  delayed dynamic-bank selectors. Required storage is rounded up by II, then
  to a divisor of the existing controller ring size so wraparound preserves
  iteration identity. One slot emits a scalar stage register, not a
  dynamically indexed context array.
- Proven distance-one recurrence PHIs use the producer's actual feedback
  write edge. Unknown recurrences retain the full ring. Results read only at
  final retirement may retain just the latest iteration. This is not general
  register sharing between unrelated SSA values.
- II=1 pipelines whose stored values all fit one slot use fixed-stage
  iteration tags. Address generation no longer selects an iteration counter
  through a dynamic slot mux. Valid bits and tags freeze together on `ap_ce`.
  Other pipelines retain the original ring controller.
- DATAFLOW external memory `ce` and scalar output validity are qualified by
  the owner's clock enable and reset. Raw internal request signals remain
  visible to backpressure analysis, avoiding a combinational enable loop.
  This prevents a stalled task from issuing new external requests and
  overwriting its unconsumed one-cycle RAM response.

The code does not claim arbitrary-latency elastic external-memory adapters,
general cross-iteration bypassing, unrestricted loop flattening, or a complete
latency/resource optimizer. Heuristic instruction delays still need validation
with the generated RTL under the target clock constraint.

## Reports

`schedule.json` reports `forwarded_block_load_count`. Pipeline JSON/RPT reports
include `forwarded_load_count`, `stage_indexed_controller`, per-value storage
slots and write/read edges, `retirement_only`, `value_storage_bits`, and
`uncompacted_value_storage_bits`. The latter two count pipeline SSA-value
storage, not controller tags, RAM, or measured FPGA FF utilization. The binding
report counts the emitted controller and payload registers separately from
post-synthesis Vivado QoR.

## Validation

The unified regression driver now includes `PipelineStorage`:

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j4
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/lighthls-storage-regressions --keep-going
```

The new suite checks repeated transactions without reset, pointer-dependent
loads with multiple sampling times and ring wraparound (including a two-slot
value in a four-slot controller), one- and two-cycle
RAM reads with static and data-dependent banks, simultaneous dual-port
forwarding, and depth-one FIFO backpressure
with fast pipelined producers and a slower sequential consumer. The
backpressure fixture exposed an external-memory clock-enable bug during
development; the failing intermediate artifacts are retained outside Git.
The seven test configurations check 14,336 output values in total.

`Tests/PipelineStorage/run_app_smoke.py` independently runs the 18 existing
pipeline, partition, and DATAFLOW App testbenches without tying functional
success to a particular schedule size. Exact schedule assertions remain in
the regular suites.

`Tests/FairComparison/compare_u250_baselines.py` audits changes against a
same-source checkpoint. It preserves mixed tradeoffs explicitly instead of
combining LUT and FF into an arbitrary area score. Invalid or incomparable
measurements and timing failures cannot pass the audit. The optional strict
mode also rejects any increased cycle/resource metric. Its adversarial unit
tests are included as `FairComparisonQoR`, bringing the local driver to ten
suites.

## Remaining optimization directions

1. Fold pipeline entry/launch/drain edges and constant address-base setup when
   input sampling, PHI initialization, and memory request timing can be proven
   equivalent. Short kernels still pay a disproportionate fixed control cost.
2. Make storage selection aware of synthesized FF versus LUTRAM cost. In the
   memory-II case, the previous implementation used 40 LUTs as distributed
   memory; the new version uses no LUT memory and retains one 32-bit stage
   register. Total LUT falls from 134 to 91 while FF rises from 42 to 71.
   Fewer logical storage bits alone are not a complete FPGA-area objective.
3. Coalesce recurrence feedback and retirement-only results when they are
   provably the same value, including initialization and repeated calls.
   Current scalar stage allocation does not share unrelated SSA values.
4. Reduce DATAFLOW queue/control and partition-bank selection overhead without
   weakening task-clock-enable or bounded-channel backpressure semantics.
   FIFO/PIPO latency and resource optimization must remain separate objectives.
5. Broaden the correctness corpus before expanding the supported language and
   pragma subset. The capability matrix distinguishes supported, rejected,
   and empty App directories; it is not an equivalence proof for every App.

These are backend priorities. A future RL reward should consume only verified,
same-contract measured QoR, preserve explicit resource tradeoffs, and treat
failed compilation/simulation/synthesis as invalid samples rather than zero
cost. This checkpoint does not implement or train that RL policy.

## Measured checkpoint

The [complete 20-case archive](baselines/u250_100mhz_storage_20260911/README.md)
uses one compiler SHA256, Vitis/Vivado 2025.2, U250, and a 10 ns constraint.
All comparisons pass the common functional and normalized-interface checks;
all Light-HLS designs meet the Vivado OOC synthesis constraint. All 20 RTL
inputs regenerate byte-for-byte from the final working-tree executable.
Ten local regression suites and the separate 18-case App smoke pass.

Selected measurements below compare the previous September 8 checkpoint
with this checkpoint. LUT/FF are normalized common-flow measurements, not
pre-backend estimates or structural register counts.

| Case | Previous cycles | Current cycles | Vitis cycles | Previous LUT/FF | Current LUT/FF | Vitis LUT/FF |
|---|---:|---:|---:|---:|---:|---:|
| Simple pipeline | 22 | 21 | 16 | 74 / 40 | 38 / 25 | 9 / 11 |
| Recurrence | 22 | 21 | 16 | 141 / 200 | 53 / 121 | 72 / 39 |
| Outer pipeline | 22 | 21 | 17 | 193 / 39 | 73 / 36 | 58 / 52 |
| Cyclic partition | 10 | 9 | 4 | 211 / 30 | 123 / 17 | 96 / 7 |
| Complete partition | 5 | 3 | 0 | 110 / 135 | 110 / 5 | 92 / 0 |
| FIFO DATAFLOW | 267 | 137 | 132 | 216 / 262 | 215 / 168 | 164 / 54 |
| PIPO DATAFLOW | 780 | 396 | 388 | 395 / 369 | 377 / 273 | 275 / 126 |
| Local partition | 96 | 60 | 42 | 587 / 342 | 589 / 310 | 242 / 152 |

Across all 20 cases, twelve improve without increasing any measured cycle or
resource metric, seven have mixed tradeoffs, and one is unchanged. Seventeen
reduce cycles; three retain their cycle count. The strict no-metric-regressions
gate does not pass. In addition to the memory-II example discussed above,
multidimensional partitioning changes from 271 LUT / 25 FF to 183 LUT / 143 FF,
and partitioned DATAFLOW changes from 550 LUT / 346 FF to 477 LUT / 353 FF.
These results are not hidden by an aggregate area score. The scalar case stays
at 15 cycles versus Vitis's 4, and short pipelines retain fixed control costs.
This is a validated improvement checkpoint, not universal Vitis parity.

Working experiments are under `/tmp/lighthls-storage-qor-20260911.K7cKW9/`.
Four initial Vivado comparison failures were preserved and rerun successfully
without changing compiler or inputs. Their cause remains unproven. Only fresh,
complete measurements enter the archive; neither simulation-only results nor
intermediate estimates replace synthesis QoR. OOC timing is not post-route
board timing or a measured Fmax for purely combinational paths.
