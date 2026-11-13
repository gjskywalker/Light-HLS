# Clock-aware scheduling and RTL cost reduction

This update targets avoidable cycles and logic in the existing backend. It
does not replace the scheduler with SDC, change source pragmas, or claim
general QoR parity with Vitis HLS. The immutable Phase-1 measurements remain
under `docs/baselines/u250_100mhz/`.

## Implemented changes

### Scheduling and dependence semantics

`HI_OperationScheduling` preserves zero-cycle cast/address operations and
chains integer operations when their characterized delay fits 70% of the
requested clock period. The remaining margin is reserved for control,
routing, and register overhead. Unknown or multicycle arithmetic retains
a register. Cross-basic-block users and PHI feedback remain registered;
chains that exceed the budget are broken at producer registers.

Ordinary basic blocks now preserve potentially aliasing RAW, WAR, and WAW
**request-edge** order, not just port capacity. Disjoint banks and provably
different constant-offset addresses remain independent; read/read pairs
remain independent. A read request must precede a conflicting write, but its
later data capture does not unnecessarily delay that write. Existing
distance-aware modulo constraints continue to govern pipelined loops.

Scheduling reports distinguish `memory_ordering_cycles` from
`memory_port_serialization_cycles`. They also expose
`combinational_value_count` and `combinational_delay_budget_ns`.

### Binding and Verilog generation

Chained SSA values become sized wires or inline SystemVerilog expressions,
not dedicated registers, pipeline context arrays, or independently muxed
functional units. Explicit result-width casts preserve truncation, extension,
signed comparison, and arithmetic-shift semantics inside pipeline expressions.
Single-operation functional units have direct operand wires; only genuinely
shared units retain state-selected operand muxes. Combinational declarations
follow IR order, reset assignments follow RTL-name order, and DATAFLOW channels
follow source-name order for deterministic emission.

Pipeline retirement no longer includes a redundant cycle beyond the latest
operation completion. Synchronous RAM request/capture latency is unchanged.

### Continuous execution of a proven perfect nest

`HI_LoopPipelineAnalysis` can lower a perfect two-loop nest into one continuous
hardware pipeline. The original LLVM IR remains unchanged. The hardware
iteration count becomes the product of the two constant trip counts, and the
pipeline exits directly to the outer exit. Static-latency analysis traverses
these hardware edges rather than the original backedges.

This optimization requires all of the following:

- One canonical single-block inner pipeline, one outer header and one outer
  latch, and unconditional entry into the inner body.
- No row-level memory effects, side effects, or escaping values from the nest.
- Every hardware-relevant inner PHI is affine, starts from an outer affine
  PHI, and satisfies `outer_step == inner_step * inner_trip_count`.
- No direct datapath use of a resetting inner index or an outer row counter.

Other nests retain their original drain/restart behavior. Nested PIPELINE
directives remain unsupported. Reports record `source_trip_count`,
`flattened_loop_count`, the effective `trip_count`, and `flattening_reason`.
This is backend continuous-nest lowering, not unrestricted LLVM loop flattening.

### DATAFLOW

The timing evaluator exports both integer cycle latency and characterized
delay for instructions in all evaluated functions. `HI_DataflowTaskGeneration`
passes this model into child backends, so task-local chaining uses the same
target-clock policy as the top-level backend. FIFO/PIPO capacity, backpressure,
clock-enable stalls, transaction queues, and channel ownership are unchanged.
Graph timing is recomputed from the optimized child schedules.
Task timing traverses the hardware control-flow edges and counts folded
outer loops only once. A dedicated nested PIPELINE + partitioned PIPO test
checks that the exact graph estimate matches the simulated transaction cycle
count, including the testbench's one-cycle launch-counting convention.

## Validation

All eight unified local regression suites pass. This includes the 37-directory
App capability matrix; compilation support is still distinct from functional
coverage. Additional regressions cover:

- A dynamic same-object WAR hazard with both aliasing and nonaliasing indices,
  using 64 transactions without resetting the DUT.
- Continuous execution of the existing `backend_pipeline_inner` App case,
  plus a row-dependent nest that must not be flattened.
- Truncation, sign extension, signed comparison, and arithmetic shifts through
  chained pipeline expressions: 256 values at each of 100 MHz and 200 MHz.
- Existing recurrence, memory-II, partitioned-memory, storage-latency,
  persistent DATAFLOW, partial PIPO, broadcast, initialized-cycle, and
  conditional-FIFO tests.
- Nested continuous PIPELINE inside a DATAFLOW producer, composed with cyclic
  partitioning and a pipelined consumer: 46 reported cycles and 47 testbench
  cycles, with matching output data.

The library build script now stops on compilation failure instead of staging
partially rebuilt archives. Build and run regressions with:

```bash
cmake -S Light_HLS/Tests/Light_HLS_Top \
  -B Light_HLS/Tests/Light_HLS_Top/build -DHI_USE_LLD=ON
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j4
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/lighthls-qor-regressions --keep-going
```

Do not overlap builds into the same build/staging directories. Configure
`LLVM_DIR` and `Polly_DIR` explicitly if they are not discoverable locally.

## Measured U250 results

Validation completed on September 9, 2026. The comparison includes all 19
original baseline designs and one additional nested PIPELINE + partitioned
DATAFLOW design. All 20 match their Vitis result traces and normalized
interfaces and complete the common Vivado OOC flow at a 10 ns constraint.
All seven original pipeline cases match Vitis's **loop II**, which must not be
confused with top-level transaction latency or reacceptance interval.

Representative results are below. LUT/FF are measured from generated RTL using
the same normalized-wrapper flow, not the HLS estimator.

| Design | Previous Light cycles | Current Light cycles | Vitis cycles | Previous Light LUT / FF | Current Light LUT / FF | Vitis LUT / FF |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Scalar | 30 | 15 | 4 | 367 / 478 | 157 / 269 | 106 / 102 |
| Simple pipeline | 25 | 22 | 16 | 146 / 69 | 74 / 40 | 9 / 11 |
| Inner pipeline | 64 | 24 | 16 | 169 / 73 | 94 / 40 | 20 / 20 |
| Memory dependence | 38 | 36 | 29 | 104 / 49 | 84 / 39 | 14 / 11 |
| Cyclic partition | 15 | 10 | 4 | 322 / 103 | 211 / 30 | 96 / 7 |
| FIFO DATAFLOW | 460 | 267 | 132 | 256 / 369 | 216 / 262 | 164 / 54 |
| PIPELINE + partition DATAFLOW | 52 | 45 | 34 | 724 / 412 | 550 / 346 | 223 / 175 |

The inner-pipeline and memory-dependence "previous" columns use the immediately
preceding dependence-only checkpoint, not the older Phase-1 snapshot (124 and
52 cycles respectively). Other previous values come from the immutable Phase-1
archive. Testbench-local scalar cycle counts can differ by one from the
normalized transaction measurement above; these conventions are not mixed.

General Vitis parity has **not** been reached. Several simple array cases have
lower Light-HLS LUT or FF counts individually, but latency and total resource
tradeoffs still favor Vitis in most examples. In particular, matching loop II
does not eliminate startup/drain cycles or the cost of iteration-context storage.

The compact measured archive is
[`u250_100mhz_qor_20260908`](../baselines/u250_100mhz_qor_20260908/README.md).
Raw projects and the final local regression remain in
`/tmp/lighthls-qor-20260908.71Q3iI/`.

`verify_archived_rtl.py` checks the final compiler against measured inputs.
It preserves the original measurement/compiler fingerprints and permits only
reordering adjacent, independent literal-zero nonblocking assignments. It
does not perform general equivalence checking or relabel a cached measurement
as a new compiler run. The multi-channel FIFO and PIPO cases are remeasured
after stabilizing channel order instead of broadening this comparison rule.

## Remaining priorities

1. Memory-response bypass and lifetime-based pipeline storage: avoid a second
   capture register when a registered memory response can feed its consumer
   directly, and replace whole-iteration context arrays with stage registers or
   smaller lifetime-specific buffers where proven safe. Both optimizations
   must preserve response availability and FIFO/PIPO stall semantics.
2. Reduce slot/iteration selection and address mux logic, preserving memory
   request timing and global stall behavior.
3. Cost-aware sharing and short-loop unrolling, accounting for mux/fanout cost
   rather than minimizing functional-unit count alone.
4. Generalize continuous nests only with proofs for row resets, non-affine
   recurrences, and live-outs; keep conservative fallbacks.
5. Characterize real multicycle/pipelined functional units and routing-aware
   timing. The chain budget is a heuristic, not a timing-closure guarantee.
6. Consider SDC or a stronger modulo scheduler after these hardware costs are
   modeled, using measured generated-RTL QoR as the comparison boundary.

Vivado OOC synthesis is not routed implementation or on-board timing sign-off.
