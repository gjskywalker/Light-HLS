# U250 Light-HLS/Vitis HLS fair comparison

This harness compares RTL generated from one source file by Light-HLS and Vitis
HLS. U250 is the fixed primary target. Both RTL implementations are synthesized
by the same generated Vivado out-of-context Tcl flow with the same top function,
part, and clock constraint.

The current flow is `vivado_ooc_normalized_v3_pre_synth_io20`. It loads a
clock/IO constraint script with `read_xdc -unmanaged` **before** synthesis.
Both generators receive maximum input and output delays of 20% of the period
and minimum delays of zero. These are explicit kernel-boundary assumptions,
not measured U250 board delays. Older `v2` archives remain unchanged and must
not be mixed with `v3` results. Re-run the frozen old compiler under `v3` when
measuring a compiler improvement.

Add `--implement` to place and route both normalized designs. `metrics.txt`
and `post_synth.dcp` remain synthesis-only; `route_metrics.txt`,
`post_route.dcp`, `route_utilization.rpt`, `route_timing_summary.rpt`,
`route_status.rpt`, and `route_drc.rpt` describe implementation. Reported
implementation timing must have no routing errors, unconstrained endpoints,
or missing IO delays, and must meet both setup and hold. OOC routing does not
include a board shell or a physically specified clock tree. Period minus WNS
includes the external timing budget and must not be advertised as Fmax.

For the DSP pressure fixtures, compare sustained traffic separately:

```bash
python Light_HLS/Tests/FairComparison/run_sustained_throughput.py \
  --comparison /path/to/comparison/u250_fair_comparison.json \
  --output /tmp/sustained-comparison
```

This runs the unchanged generated RTL in XSim, using the same 32 held-start
offers, changing scalar arguments, immutable input RAM and a native C oracle.
It checks all 8192 writes, drain behavior and transaction ordering. Steady
throughput uses final-commit intervals after four warmup intervals; ready
acknowledgements and done edges are recorded separately. XSim requires local
IPC permissions. The existing Light-only queued/stalled test remains a
protocol stress test, not a cross-generator throughput measurement.

Boundary follow-up:

- With minimum input delay zero and no physical OOC port locations, direct
  port-to-argument-register paths can fail hold on both generators. Such
  measurements stay `reward_eligible: false`; internal register-to-register
  timing is a separate diagnostic, not a waiver. `report_ooc_paths.tcl`
  accepts a routed DCP and an output directory for this diagnostic.
- The September 29 acceptance fix aligns ordinary/pipelined scalar capture with
  the `ap_start && ap_ready` edge. The previous `dsp_map` failure remains a
  negative baseline. The pressure suite now includes continuous transactions
  and reset-abort recovery, in addition to DATAFLOW queue/stall tests.

For physical correctness checks in a **different, explicitly registered**
context, use:

```bash
python3 Light_HLS/Tests/FairComparison/run_registered_context.py \
  --comparison /path/to/cached/u250_fair_comparison.json \
  --light-hls-top /path/to/frozen/Light_HLS_Top \
  --output /tmp/fresh-registered-context
```

This supports the unchanged DSP map, banked BRAM, and DSP DATAFLOW pressure
fixtures. Cached Vitis RTL is reused; Light-HLS RTL is regenerated. Both run the
same 32-offer synthesizable stimulus, one-cycle immutable RAM response, and
retained output observers. XSim checks all 8192 outputs against native C before
Vivado synthesis and routing. The physical shell has only a clock port and an
instantiated BUFG; every data boundary is an actual on-chip path. Only the
clock buffer is idealized for functional simulation. No extra memory latency,
false path, multicycle exception, or relaxed hold limit is introduced.

The distinct flow ID is `vivado_registered_pressure_context_v1`. It reports
total, kernel-hierarchy, and shell resources separately, preserves DCPs and
timing/coverage reports, and never changes old virtual-IO eligibility. This is
bounded on-chip subsystem validation, not an AXI/PCIe board shell, arbitrary
traffic proof, or a replacement for standalone kernel QoR measurements.
`--simulate-only` explicitly leaves `physical_valid` false.

After routing, independently simulate the mapped netlist (repeat for both
generators):

```bash
python3 Light_HLS/Tests/FairComparison/verify_registered_netlist.py \
  --context /tmp/fresh-registered-context --generator light_hls
```

This exports the routed checkpoint in functional-simulation mode and runs the
same 8192-output oracle with Vivado UNISIM and global-startup models. It is
post-route functional simulation, not SDF simulation; setup and hold are
validated by the separate routed static timing reports.

The inline tables below are historical bring-up measurements. The latest
pipeline-boundary checkpoint is documented in
[the September 14 optimization notes](../../../docs/RTL_BOUNDARY_OPTIMIZATION_20260914.md),
with compact measurements in
[the corresponding archive](../../../docs/baselines/u250_100mhz_boundary_20260914/README.md).

The comparison contract checks:

- identical source file and top-function name;
- U250 part `xcu250-figd2104-2L-e` and one clock period;
- Vitis automatic loop pipelining disabled, while explicit source pragmas remain
  available to Vitis;
- native post-synthesis top-level signatures for auditability;
- identical normalized post-synthesis signatures through generated common
  wrappers;
- optional execution of one self-checking Icarus testbench against both RTL
  implementations;
- measured LUT, FF, DSP, BRAM tile, worst slack, and critical-path delay from
  one common Vivado flow.

Run the checked scalar baseline:

```bash
Light_HLS/Tests/FairComparison/run_backend_scalar_u250.sh \
  /path/to/Light_HLS_Top \
  /tmp/backend-scalar-u250-fair
```

The output root contains `u250_fair_comparison.json`, generator logs, both RTL
trees, generated wrappers, Icarus logs, Vivado reports, metrics, and
checkpoints. The JSON is `reward_eligible` only when the normalized synthesized
interfaces match and the common RTL testbench produces identical `RESULT`
traces and both normalized designs meet synthesis timing. When implementation
is requested, the routed timing/coverage checks must pass as well. QoR deltas
use the normalized wrapper boundary; native post-RTL metrics
and native interface equivalence remain separate audit fields.

Light-HLS artifacts follow the normal layout under
`light_hls/reports`, including the human-readable `report.rpt` index and the
hidden `.lighthls` intermediate directory.

For another supported design, invoke `run_u250_fair_comparison.py` directly and
provide the source, top name, and optionally a common testbench. A Light-HLS
configuration can be passed with `--config`. The wrapper builds a superset
`ap_ctrl_hs` contract, ties absent output ports to zero, and maps Vitis
multidimensional complete-partition names to Light-HLS row-major flattened
names using `binding.json`. Direction or width conflicts on a canonical port
are still hard failures.

The harness accepts repeatable `--testbench-define` options for both RTL
simulations and `--vitis-testbench-define` options only for the Vitis RTL
simulation. These defines support a testbench-side naming adapter without
changing either synthesized native top. Native synthesized port signatures are
still compared and reported independently; they do not by themselves make a
functionally verified normalized comparison ineligible for reward.

The common flow compares generated wrapper tops around native HLS modules rather
than whole accelerator shells. AXI interconnect, host integration, placement,
routing, power, and board shell overhead are outside the current comparison
scope. Vitis `csynth.xml` values remain labeled as estimates; normalized common
post-RTL Vivado results are the measured comparison values.

Cycle counts use each common testbench's completion observation. They are not
automatically the last external-memory commit edge: a combinational done
signal may precede that edge, while a registered done signal may follow it.
`pipeline_event_probe.v` can wrap the unchanged simple pipeline App testbench
to expose this distinction for either generated RTL implementation. See the
September 14 notes for the measured event timeline. It is a separate diagnostic
and does not alter archived benchmark sources, testbenches, or reward fields.

## Checkpoint regression audit

After archiving a new run, compare it with the previous checkpoint:

```bash
python3 Light_HLS/Tests/FairComparison/compare_u250_baselines.py \
  --baseline docs/baselines/u250_100mhz_qor_20260908/u250_baselines.json \
  --candidate /tmp/new-u250-archive/u250_baselines.json \
  --output /tmp/new-u250-qor-changes.json
```

The audit rejects missing/duplicate cases, changed source/testbench/config
fingerprints, different clocks or synthesis contracts, invalid functional or
interface results, and missing metrics. It reports improvements, regressions,
unchanged cases, and mixed cycle/resource tradeoffs separately. Clock failures
produce a nonzero exit status. `--require-no-regressions` additionally rejects
any increase in cycles, LUT, FF, DSP, or BRAM, even when another metric improves.
Without that flag, a successful audit means the data are comparable and the
clock is met, not that every optimization is a Pareto improvement. Native
interfaces and heuristic HLS estimates are not substituted for normalized QoR.

## Verified scalar baseline

The checked `backend_scalar` run with Vitis/Vivado 2025.2 produced matching
interfaces and four matching functional traces. Both implementations met the
10 ns U250 clock in the common OOC flow:

| Generator | LUT | FF | DSP | WNS (ns) | Critical path (ns) | Testbench cycles |
|---|---:|---:|---:|---:|---:|---:|
| Light-HLS | 367 | 478 | 0 | 8.559 | 1.441 | 30 |
| Vitis HLS | 106 | 102 | 0 | 8.735 | 1.265 | 4 |

This baseline exposes the current optimization gap without mixing Vitis HLS
estimates with post-RTL measurements. It is not a claim that the two compilers
already apply equivalent scheduling policies: closing that gap is the purpose
of the next pipeline and binding work.

## Verified pipeline App cases

Run all supported pipeline examples sequentially with:

```bash
Light_HLS/Tests/FairComparison/run_pipeline_u250_comparisons.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/backend-pipeline-u250-comparison \
  --clock-period-ns 10
```

Use `--case <name>` to run selected cases. `--resume` reuses complete results
and preserves an incomplete case directory before retrying it. The aggregate
outputs are `u250_pipeline_comparison.rpt` and
`u250_pipeline_comparison.json`.

The App sources explicitly select single-port `ram_1p` memories so Vitis and
Light-HLS schedule against the same memory-port capacity. Vitis automatic loop
pipelining remains disabled; only the source PIPELINE directive is active. One
self-checking testbench supplies both native RTL implementations with identical
memory contents. Light-HLS uses Vitis-compatible `_r` suffixes for reserved
parameter names and emits direction-aware `ap_memory` data signals, so no
testbench-side naming adapter is required for these cases.

The verified U250/Vivado 2025.2 results at 10 ns are:

| Case | Light II | Vitis II | Light cycles | Vitis cycles | Light LUT | Vitis LUT | Light FF | Vitis FF | Light WNS | Vitis WNS | Functional | Exact interface |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| II=1 | 1 | 1 | 25 | 16 | 146 | 9 | 69 | 11 | 8.490 | 9.318 | pass | yes |
| Memory-limited | 2 | 2 | 42 | 32 | 169 | 53 | 68 | 47 | 8.487 | 9.305 | pass | yes |
| Recurrence | 1 | 1 | 24 | 16 | 215 | 72 | 285 | 39 | 8.447 | 9.006 | pass | yes |
| Memory dependence | 2 | 2 | 38 | 29 | 104 | 14 | 49 | 11 | 8.924 | 9.396 | pass | yes |
| Inner pipeline | 1 | 1 | 64 | 16 | 169 | 20 | 73 | 20 | 8.481 | 9.311 | pass | yes |
| Outer pipeline | 4 | 4 | 25 | 17 | 377 | 58 | 50 | 52 | 8.026 | 9.263 | pass | yes |
| Mixed memory | 2 | 2 | 25 | 15 | 127 | 11 | 47 | 9 | 8.490 | 9.454 | pass | yes |

Cycle counts are observed from `ap_start` through `ap_done` in the common RTL
testbench. All entries passed exact result-trace comparison, exact native
interface comparison, and common Vivado OOC synthesis. The aggregate result is
therefore reward-eligible under the strict interface and functional contract.
Read-only memories expose `address/ce/q`, write-only memories expose
`address/ce/we/d`, and mixed memories expose the complete signal set in both
generators.

The achieved II now agrees with Vitis in all seven cases. The exact WAR case
uses read-request to write-request ordering instead of waiting for load-data
completion, reducing Light-HLS from II=3 to II=2. The inner-loop case preserves
the surrounding-loop address recurrence as a symbolic invariant, proving that
successive inner-loop addresses are disjoint and reducing II from 6 to 1.
Both optimized cases pass exact result-trace, native-interface, and common
Vivado OOC checks. The remaining cycle gaps come from pipeline depth and nested
loop control: Light-HLS drains and restarts the inner pipeline for every outer
iteration instead of flattening or rewinding the perfect loop nest.

## Array-partition batch

Run the cyclic, block, complete, complete read/write, multidimensional, local,
and complete-partition pipeline cases with:

```bash
Light_HLS/Tests/FairComparison/run_array_partition_u250_comparisons.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/backend-array-partition-u250-comparison \
  --clock-period-ns 10
```

Use `--case <name>` for a selected case and `--resume` to reuse completed
results. The aggregate outputs are `u250_array_partition_comparison.rpt` and
`u250_array_partition_comparison.json`.

## DATAFLOW batch

Run the FIFO-stream, PIPO-array, and combined DATAFLOW/PIPELINE/
ARRAY_PARTITION comparisons with:

```bash
Light_HLS/Tests/FairComparison/run_dataflow_u250_comparisons.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/backend-dataflow-u250-comparison \
  --clock-period-ns 10
```

The shared testbench supplies a small Vitis-only naming adapter for dual-port
memories. It does not modify either generated RTL implementation. Each case
must pass two transactions, normalized post-synthesis interface comparison,
and the common U250 Vivado OOC flow. The aggregate outputs are
`u250_dataflow_comparison.json` and `u250_dataflow_comparison.rpt`.

## Complete U250 baseline

Run every Phase-1 comparison and build one compact archive with:

```bash
Light_HLS/Tests/FairComparison/run_u250_baselines.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/dapo-ii-u250-baselines \
  --clock-period-ns 10 \
  --resume \
  --keep-going
```

The command covers scalar, pipeline, array-partition, and DATAFLOW cases.
`--resume` is recommended because Vitis and Vivado runs are long and a
completed case is never repeated. An incomplete case directory is renamed
before retry so its diagnostic logs remain available.

The `normalized` directory contains `u250_baselines.json`, CSV, and a readable
RPT table. Every row includes source/config/testbench hashes, the Light-HLS
binary hash, a comparison identifier, functional status, cycles, II, and
post-RTL Vivado QoR. Raw Vitis/Vivado projects should remain outside Git; only
the compact archive is intended for version control and subsequent RL reward
ingestion.

## Final-compiler RTL identity checks

`verify_archived_rtl.py` regenerates the inputs to completed comparison reports
and checks exact RTL equality, allowing only the ordering of adjacent
independent literal-zero nonblocking assignments. It retains raw hashes and
does not rewrite measured executable fingerprints or reward eligibility.
Explicit-config comparisons are currently rejected because their original
configuration path is not recorded in older reports. Run its narrow
normalization tests with `python3 test_rtl_identity.py` from this directory.

Use `--resume` only with unchanged inputs and compiler, unless auditing and
retaining the original per-case fingerprints explicitly. The existing batch
runners check completion flags, not every current input hash; a fresh output
directory is required for a new unverified compiler/configuration revision.
