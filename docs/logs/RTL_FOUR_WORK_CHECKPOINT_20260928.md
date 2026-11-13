# Pipeline, DATAFLOW, binding, and pressure-validation checkpoint

This completes four bounded workstreams following the
[scalar/channel checkpoint](RTL_SCALAR_AND_CHANNEL_OPTIMIZATION_20260922.md).
It does not claim universal Vitis parity or a globally optimal scheduler.
All new code comments and documentation are in English.

## 1. Pipeline controller and transaction boundaries

The compact stage-indexed controller now covers legal II-greater-than-one
pipelines when each context value needs one slot and scalar recurrences are
exact. The phase counter still enforces II. More general context rings remain
available as the conservative fallback.

Iteration tags are kept through their actual last sampling edge, including
derived addresses and dynamic-bank responses. A last-token bit chain replaces
retirement-only tags only when the complete chain uses fewer bits. This guard
avoids the LUT/FF regression of an unconditional last-token implementation.

An eligible pure terminal return can capture the final FU result on the
retirement edge. The externally visible return remains registered and holds
its previous value until completion. A direct FU feeding both recurrence
feedback and the final return gets a narrowly scoped `keep` result-net hint:
without it Vivado duplicated the recurrence adder, raising this example to
84 LUTs; preserving the shared bound result reduces it to 51 LUTs.

Runtime scalar seeds are sampled from live ports on accepted start; later
execution uses captured arguments. Adjacent pipelines can use a unique
conditional entry predecessor rather than requiring an unconditional LLVM
preheader. The successor initializes its own PHIs, and predecessor live-outs
retain global storage. A two-loop, runtime-seeded recurrence regression checks
this boundary explicitly. General overlapping nested PIPELINE support has not
been added.

## 2. DATAFLOW handoff and completion

The top wrapper exposes its retirement predicate directly instead of adding
another registered completion cycle. Child task completion remains registered
after the task's effects. Existing same-edge retirement/acceptance accounting,
two-outstanding-transaction queues, and stall-qualified effects are preserved.

Completion is not assumed to follow solely from graph sinks. The shared
`dataflowCompletionTasks` analysis omits a producer's completion credit only
when an acyclic, balanced, positive-rate, non-initialized outgoing channel
proves that consumer completion implies producer completion. A PIPO ownership
transfer provides that proof; a FIFO requires exact timing with its final
token on the producer's final enabled edge. Other producers are tracked too;
cyclic graphs conservatively track every task. RTL and performance analysis
use the same completion set, reported as `completion_tasks`.

The new trailing-effects test emits the last FIFO token before 64 public RAM
writes. Both natural and injected-stall execution must wait for those writes.
This safety fallback explains the modest extra completion-credit cost in the
private-buffer App case; it is not hidden as a resource improvement.

## 3. Cost-aware binding

For each existing operator group, binding compares deterministic ordinal
allocation with operand-affinity allocation across nonconcurrent time slots.
It adopts affinity only for a strict complete-group decrease in
`selector_input_bits_v1`. Identical constant or captured-argument inputs on a
shared FU bypass the input selector. FU count, operation schedule, memory-port
assignment, and established storage lifetime rules remain unchanged.

The cost is a proxy for selector inputs, not a calibrated LUT estimate or an
SDC/resource-budget optimizer. JSON and human binding reports expose ordinal
and selected costs, rebound groups, and invariant operands.

The 250 MHz binding fixture has selector cost **256 -> 128**. Actual common
Vivado OOC synthesis, with both versions passing 256 changing-input calls:

| Metric | Before | After |
| --- | ---: | ---: |
| LUT | 161 | 129 |
| FF | 294 | 230 |
| DSP | 6 | 6 |
| WNS at 4 ns | +0.263 ns | +0.342 ns |

## 4. Broader benchmarks and clock coverage

Four new 256-word designs exercise a runtime multiply/add, local BRAM with
wrapped addressing, two partitioned BRAM banks, and a three-task DSP/FIFO
DATAFLOW graph. All four are physically compared at 100 and 250 MHz; the DSP
map also runs at 150 and 200 MHz. Both RTL implementations must match the same
native C++ reference. The software queue adapter is used only by that reference,
not by either synthesis frontend.

The wider tests also drove two robustness fixes:

- Memory-depth inference respects finite-width truncation and extension.
  A wrapped 8-bit address into a 256-element allocation is not misinterpreted
  as requiring 18,616 elements.
- Off-grid clock requests select a characterized period no slower than the
  request. Uncovered faster clocks fail with `HLS_LIBRARY_CLOCK_UNSUPPORTED`
  and exit code 1, rather than asserting or fabricating a slower model.

The profiling workflow uses the existing common Vitis/Vivado comparison flow.
The constraint-sweep skill provided the 150/200/250 MHz plan; the measured
summary is generated from actual results, not the skill's plan-only summary.

### Existing App comparison

U250 `xcu250-figd2104-2L-e`, 100 MHz, Vitis/Vivado 2025.2, unchanged benchmark
sources and testbenches, common normalized-wrapper OOC synthesis:

| Case | Light cycles before -> after | Vitis cycles | Light LUT before -> after | Light FF before -> after |
| --- | ---: | ---: | ---: | ---: |
| Memory II=2 | 34 -> 34 | 32 | 75 -> 70 | 56 -> 56 |
| Outer pipeline | 18 -> 18 | 17 | 63 -> 55 | 23 -> 23 |
| Mixed-memory pipeline | 17 -> 17 | 15 | 36 -> 36 | 19 -> 16 |
| Recurrence | 19 -> 18 | 16 | 49 -> 51 | 87 -> 80 |
| Multidimensional cyclic partition | 6 -> 6 | 4 | 128 -> 122 | 79 -> 76 |
| FIFO DATAFLOW | 132 -> 131 | 132 | 198 -> 198 | 64 -> 63 |
| PIPO DATAFLOW | 387 -> 386 | 388 | 337 -> 337 | 149 -> 148 |
| Pipeline/partition DATAFLOW | 37 -> 36 | 34 | 233 -> 234 | 174 -> 173 |
| Nested DATAFLOW | 39 -> 38 | 34 | 238 -> 238 | 176 -> 175 |
| Private-buffer DATAFLOW | 391 -> 390 | 389 | 300 -> 310 | 324 -> 330 |

The complete [21-case archive](../baselines/u250_four_work_20260928/app/u250_baselines.csv)
contains seven improvements with no cycle/resource increase, eleven unchanged
cases, and three explicit tradeoffs. No case has a cycle regression. Recurrence
adds two LUTs for one fewer cycle and seven fewer FFs; pipeline/partition
DATAFLOW adds one LUT for one fewer cycle and FF; private DATAFLOW adds ten LUTs
and six FFs while removing one completion cycle and preserving conservative
completion tracking. This is not a no-resource-regression checkpoint.

### New pressure comparison

The following latency is **last committed memory write minus initial launch**,
not first observed done. Resource pairs are Light/Vitis at 100 MHz:

| Design | Final-commit cycles L/V | LUT L/V | FF L/V | DSP L/V | BRAM tiles L/V |
| --- | ---: | ---: | ---: | ---: | ---: |
| DSP map | 259 / 259 | 52 / 15 | 32 / 46 | 2 / 1 | 0 / 0 |
| DSP DATAFLOW | 261 / 261 | 222 / 167 | 138 / 79 | 2 / 1 | 0 / 0 |
| BRAM permutation | 515 / 515 | 117 / 94 | 91 / 86 | 0 / 0 | 0.5 / 0.5 |
| Two-bank BRAM | 771 / 515 | 166 / 111 | 133 / 78 | 0 / 0 | 1 / 1 |

At 250 MHz, DSP map is 260/259 final-commit cycles and DSP DATAFLOW is 262/261;
the two BRAM cases retain their 100 MHz cycle/resource counts. All ten pressure
points pass their constrained post-synthesis timing checks. The
[full pressure archive](../baselines/u250_four_work_20260928/pressure/u250_baselines.json)
retains done, final commit, launch intervals, resources, timing, and every loop's
II. Paced launch intervals are not a proven minimum transaction II.

For the DSP-map clock sweep, 250 MHz gives the shortest measured final-commit
time (1,040 ns) among tested timing-met points. It is not the area-minimal point:
100/150 MHz use fewer FFs. The 200 MHz result is also retained despite its
non-monotone resource cost. These are post-synthesis measurements, not routed
frequency guarantees; the common flow does not impose external I/O delays.

## Functional validation and comparison adapters

- All **24** unified regression suites pass on the final executable.
- All **18** independent App RTL smoke tests pass.
- App capabilities remain **24 passes, 12 expected rejections, one non-applicable**.
- All four pressure designs pass local Icarus/C-reference checks at
  100/150/200/250 MHz: **16 configurations**.
- The queued DATAFLOW test runs 32 transactions with natural and injected
  stalls at all four clocks: 65,536 writes, and 30 same-edge retirement/acceptance
  overlaps per configuration. This is Light-only protocol coverage, not a Vitis
  throughput comparison.
- The completion-frontier regression checks 128 calls and 10,240 writes.
- Binding runs 256 calls at each of 100/150/200/250/333 MHz, changes arguments
  after acceptance, checks return hold, and rejects an uncovered 500 MHz request.

The Vitis pressure adapter holds start until its ready sampling edge. Ordinary
RAM may have one repeated, idempotent read during a BRAM loop's exit, but input
coverage, exact unique writes, output data, and absence of later accesses remain
checked. This relaxation is not applied to Light-HLS reads or FIFO effects.

The generated Vitis DATAFLOW RTL exposes an Icarus time-zero initialization
problem in a combinational backpressure expression. Both unmodified RTL designs
are therefore additionally simulated with Vivado XSim for the DATAFLOW physical
comparisons. `xvlog --relax` accommodates existing forward wire declarations;
the synthesized RTL is not rewritten. XSim needed local IPC outside this
environment's restricted sandbox. Other comparisons use Icarus.

Failed exploratory simulations are preserved, not labeled successful.
The revalidation helper reruns corrected adapters against unchanged synthesis
artifacts, fingerprints all originals, records `synthesis_reuse`, and leaves
unknown original runtimes null. Compiler/source/testbench/config changes during
a new common comparison invalidate that run. There is no generated-RTL patch
in a final comparison: the exploratory recurrence net hint was implemented in
the backend, regenerated identically, and freshly synthesized again.

## Provenance and reproduction

Final executable SHA256:
`33476041daf7d538bc444004f3c0181ce434ec80a13479614f1b8bced46cbc5b`.

All **31** measured App/pressure RTL files regenerate byte-identically with that
executable; the binding comparison has a separate identical-RTL proof. Original
measurement compiler hashes remain intact. See the
[archive index](../baselines/u250_four_work_20260928/README.md),
[pressure instructions](../../Light_HLS/Tests/QoRPressure/README.md),
[binding instructions](../../Light_HLS/Tests/BindingCost/README.md), and
[completion regression](../../Light_HLS/Tests/DataflowCompletion/README.md).

```sh
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j8
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/new-light-hls-regressions --keep-going
```

Freeze or copy the executable before starting long comparison batches, and do
not rebuild that file while tests are running. Output directories must be new.
Raw projects/logs for this checkpoint remain under
`/tmp/lighthls-four-work-20260928.08r0cq`; compact results live in the repository.
Verification is bounded simulation and structural checking, not formal
all-input equivalence.

## Remaining measured bottlenecks

The two-bank pressure loop is still II=2 versus Vitis II=1: the bank analysis
does not yet prove the complementary banks of `i` and `i ^ 1`. The DSP map and
DATAFLOW use two DSPs versus one, motivating DSP-aware multiply/add mapping.
Small pipelines still expose controller/start/drain overhead, and registered
return-hold semantics still consume storage. The new binding proxy does not
jointly optimize latency, timing, resource budgets, and mux routing. These are
measured limitations, not claims that the four workstreams made every design
competitive with Vitis.
