# RTL correctness closure, September 29

## Scope

This checkpoint closes the known ordinary/pipelined scalar-acceptance failure
and adds an explicit physical verification context. It does not start RL,
extend the supported language/pragma subset, or establish formal equivalence
for arbitrary programs. Existing virtual-IO timing failures remain failures.

The release compiler SHA256 is
`3339196986828e95536405f2ff438b1ba7b464000829c45b17a3e66e33977f95`.
Raw runs, frozen compilers, RTL, DCPs, and logs are under
`/tmp/lighthls-correctness-20260929.kZl9Xq`.
Compact release evidence is archived under
[`baselines/u250_correctness_20260929`](baselines/u250_correctness_20260929/README.md).

## Acceptance and reset fixes

Previously an ordinary top captured arguments when it left the idle state but
acknowledged the request only at completion. A continuously asserted `ap_start`
therefore caused the next invocation to capture the previous scalar value.
The unchanged pre-fix DSP map RTL reproduces the mismatch at output 256;
Vitis passes the same traffic and C oracle.

`HI_RTLGeneration` now derives ready from the accepting idle state, with reset
and optional task clock-enable guards. The same enabled rising edge both
acknowledges `ap_start && ap_ready` and captures scalar arguments/entry PHIs.
Completion is independent. Existing pulse-start clients still work; ordinary
modules do not queue pulses offered only while busy. This interface contract
does not require Vitis and Light-HLS to produce identical ready waveforms.
It also does not allow an external RAM to be overwritten at scalar acceptance:
memory must remain valid through its required reads and final output commit.

Reset also suppresses memory CE/WE and scalar-bank valid events. Reset aborts
work; it does not undo already committed writes or clear every RAM element.
Raw DATAFLOW task memory requests remain independent of `ap_ce`: the parent
uses them to calculate stalls and gates the actual commits. An intermediate
attempt to gate raw requests by the derived enable was rejected by the stall
regressions because it introduced a combinational loop. It is not in the
release compiler.

## Functional evidence

- All 27 local regression suites pass in `release-verified`, including the
  existing FIFO/PIPO backpressure, memory event, dependence and bank proofs.
- All 18 supported App smoke cases pass.
- Six ordinary control-flow fixtures execute 1152 checked calls: 384
  single-cycle starts plus 768 continuous/gapped transactions. Coverage
  includes zero-trip loops, early returns, PHI initialization, scalar changes,
  shifts, reset interruption/recovery and final drain.
- Four pressure kernels pass at both 100 and 250 MHz. DSP map and DATAFLOW
  additionally execute 32 held-start calls and 32 post-reset recovery calls
  per frequency, checking 8192 C-reference outputs per stream. Existing
  queue-full/task-stall tests remain enabled.
- The common XSim traffic passes on Light-HLS and Vitis without editing either
  generated kernel. DSP map at 100 MHz measures final-commit intervals of
  260/256 cycles (Light/Vitis); DATAFLOW at 250 MHz measures 261/260. These are
  throughput intervals, not first-call latency or ready-to-done latency.

One complete rerun encountered a local Clang 18 frontend segmentation fault
while compiling the independent bank oracle. Its failed logs remain in
`release-complete`; a fresh, unchanged 27-suite run in `release-verified`
passes. No failed or partial run is used as the release summary.

## Physical verification boundary

The original virtual-IO flow uses minimum input delay zero and no placed input
data source. Its direct input-to-register hold failures cannot be repaired by
relabeling internal timing as full timing. The new flow is separately named
`vivado_registered_pressure_context_v1` and never changes legacy eligibility.

Both generators use the same closed on-chip harness:

- a physically instantiated BUFG and one clock constraint loaded before
  synthesis;
- retained scalar-source registers and the same 32 held-start offers;
- a one-cycle immutable memory response, with no extra address/data pipeline;
- retained observers for every meaningful kernel output;
- separate total, kernel-hierarchy and harness resource reporting.

Only the clock is an external shell port. Thus scalar, memory and output paths
are actual on-chip paths, not virtual data ports. There are no false paths,
multicycle exceptions or relaxed hold limits. The same hierarchy/retention
policy applies to both generators. Source/observer registers are retained to
prevent a closed test harness from optimizing away the kernel.

The BUFG choice follows the physical-context distinction in
[AMD UG905](https://docs.amd.com/r/en-US/ug905-vivado-hierarchical-design/I/O-and-Clock-Buffers).
Functional simulation idealizes only the clock buffer; native C checks all
8192 output values. Routing checks setup, hold, missing constraints and route
errors. This remains a bounded on-chip subsystem test, not board IO, PCIe/AXI,
DDR, asynchronous-clock integration, or a general standalone QoR replacement.

The profiling check reused Vitis synthesis reports keyed by unchanged source,
top, U250 part and period. The three hot paths are the 256-element DSP loop,
the two 256-element BRAM passes, and the DATAFLOW load/compute/store loops;
all report loop II=1. The generic profiling parser does not fully recognize
the current XML hierarchy, so loop II/depth were checked directly in child
reports. The correctness bug concerns transaction boundaries, and the old
hold failures concern external physical context, not a missing loop pipeline.

## Results and reproduction

### Routed timing results

All six implementations pass with no routing errors, unrouted nets, missing
clocks, unconstrained endpoints, or missing/partial IO constraints. Positive
slack below is measured in the new registered context only.

| Kernel | Frequency | Light setup / hold (ns) | Vitis setup / hold (ns) |
|---|---:|---:|---:|
| DSP map | 100 MHz | 7.411 / 0.035 | 7.735 / 0.048 |
| Banked BRAM | 250 MHz | 1.627 / 0.033 | 1.836 / 0.021 |
| DSP DATAFLOW | 250 MHz | 1.523 / 0.009 | 1.963 / 0.024 |

The kernel-only routed LUT/FF/DSP/BRAM counts are 46/32/1/0 versus 18/46/1/0
for DSP map, 135/91/0/1 versus 102/78/0/1 for banked BRAM, and 201/147/1/0
versus 132/79/1/0 for DATAFLOW (Light versus Vitis). Harness resources are
reported separately. These are not standalone virtual-IO QoR deltas and do
not establish general resource parity with Vitis.

All six routed functional netlists also pass the same 8192-output C oracle
using Vivado UNISIM and global-startup models. This is functional netlist
simulation, not SDF simulation; path delays are checked by the separate STA.

DRC reports retain advisory warnings about DSP pipeline-register usage and
intentionally retained observer/source nets with no downstream loads. There
are no Error/Critical Warning DRC rows. These warnings are not suppressed, and
OOC checking still does not cover all board connectivity checks.

### Commands and provenance

Only the `final-*` physical runs are release evidence. Earlier driver trials
under `registered-*` remain excluded: one exposed the rejected DATAFLOW enable
feedback, and others stopped on a Tcl command-generation bug. The command
replacement is now line-anchored and unit-tested, and the final physical runs
start from fresh output directories. Failed runs were not repaired in place.

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j8
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top /path/to/frozen/Light_HLS_Top \
  --output /tmp/fresh-regressions --keep-going
python3 Light_HLS/Tests/QoRPressure/run_tests.py \
  /path/to/frozen/Light_HLS_Top --freq-mhz 100 --freq-mhz 250 \
  --output /tmp/fresh-pressure
python3 Light_HLS/Tests/FairComparison/run_registered_context.py \
  --comparison /path/to/cached/u250_fair_comparison.json \
  --light-hls-top /path/to/frozen/Light_HLS_Top \
  --output /tmp/fresh-context
python3 Light_HLS/Tests/FairComparison/verify_registered_netlist.py \
  --context /tmp/fresh-context --generator light_hls
python3 Light_HLS/Tests/FairComparison/verify_registered_netlist.py \
  --context /tmp/fresh-context --generator vitis_hls
```

Registered-context runs require Vitis/Vivado 2025.2 and local XSim IPC access.
The driver rejects nonempty output directories, preserves failing summaries,
checks RTL fingerprints and refuses unsupported pressure sources. The archive
gate rejects partial matrices, changed artifacts and failed physical timing.
