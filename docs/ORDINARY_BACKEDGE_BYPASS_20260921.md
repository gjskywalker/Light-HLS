# Same-edge ordinary-loop recurrence updates

This checkpoint continues the [feedback-storage optimization](ORDINARY_FEEDBACK_OPTIMIZATION_20260921.md).
It targets the remaining compute-to-PHI control bubble, particularly in the
private-buffer FIFO producer and consumer. The previous change removed eligible
constant-step update registers but intentionally did not reduce loop cycles.

## Scheduling and RTL contract

An ordinary single-block loop can now sample a registered operation's bound
output directly into its self-backedge PHI on the operation's issue edge.
The normal result register is still written on that edge, including on loop
exit, so cross-block users retain the final computed value. Other users continue
to read that register; this is not a global combinational-value substitution.

Selection requires a two-input self-edge PHI and a conditional self/exit branch.
The producer must be an integer, side-effect-free operator with one scheduled
cycle and a characterized zero-cycle combinational implementation, or supported
integer wiring. Its complete selected combinational input path must fit the
existing 70%-of-period delay budget. Unknown, multicycle, memory-event, and
PIPELINE producers/regions are excluded. The delay budget remains a heuristic;
post-synthesis timing is checked separately.

The scheduler computes all other branch, successor-PHI, and side-effect readiness
requirements before committing a bypass. The final transition must coincide
exactly with the producer's issue cycle. If another operation delays the
transition, the PHI uses the retained register: a shared operator's output may
belong to a different operation on a later edge. The emitter uses the existing
bound operator output, avoiding an extra arithmetic instance. Simultaneous PHI
updates retain nonblocking, old-value semantics. FIFO request order, memory
latencies, pipeline scheduling, and DATAFLOW clock-enable gating are unchanged.

Post-RTL validation rechecks the shape, schedule edge, unique binding, pure
operator characterization, cumulative delay, and pipeline exclusions. Scheduling
JSON/RPT records `ordinary_backedge_bypass_count`, each PHI/update pair, the
sample cycle, and retained-register status. Missing schedules in failed runs
are reported without dereferencing an absent schedule-map entry. Parent DATAFLOW
reports do not aggregate these child-function bypass tables.

## Cross-block width repair

The narrow live-out fixture exposed a pre-existing invalid-IR case in the prior
compiler: a consumer located earlier in function block order had already been
processed before its producer was narrowed, leaving an i9/i32 binary operation.
The prior executable reproduces an LLVM bitcode verification failure.

`HI_VarWidthReduce` now restores the original operand type for already-processed
cross-block binary/comparison users. Unvisited users retain normal width
propagation. A broader draft introduced redundant casts and broke legacy address
analysis in four capability cases; that draft was rejected by regression and
replaced with this narrower rule. All four cases pass again. The existing PHI
and return-type repair from the preceding checkpoint remains in place.

## Validation and impact

- All 15 unified local regression suites pass, including the unchanged FIFO
  event-count, natural depth-one backpressure, private-memory freeze, and
  reset-free repeated-call checks.
- The new [OrdinaryBackedge suite](../Light_HLS/Tests/OrdinaryBackedge/README.md)
  passes 16 configurations, 1,024 calls, and 16,384 output checks. Fourteen
  comparable configurations also run the prior compiler against the same oracle:
  no cycle increases; explicit pipelines and multicycle dividers are unchanged.
  The two narrow configurations are marked non-comparable because the prior
  compiler emits invalid IR; they are not counted as baseline successes.
- All 18 independent App smoke checks pass. The 37-directory capability matrix
  retains 24 passes, 12 expected rejections, and one non-applicable entry.
- The 20-source historical audit retains 15 byte-identical RTL files. Four
  previously changed cases match the newer feedback checkpoint exactly. Together
  these establish that 19 of the existing 20 measured designs are unchanged;
  only the scalar case changes this round. The additional private-buffer FIFO
  source changes too. Both changed designs are freshly compared against Vitis.

These are bounded functional checks, not formal all-input equivalence.

## Fresh U250 comparison

Both release comparisons pass the same self-checking RTL testbench and common
normalized interface checks. All cycle/resource regression gates pass, DSP and
BRAM use stay at zero, and synthesis WNS is positive.

| Case | Light cycles before -> after | Vitis cycles | Light LUT before -> after | Light FF before -> after | Vitis LUT / FF |
| --- | ---: | ---: | ---: | ---: | ---: |
| Private-buffer FIFO | 760 -> 504 | 389 | 345 -> 329 | 510 -> 508 | 324 / 316 |
| Scalar | 15 -> 11 | 4 | 156 -> 155 | 266 -> 234 | 106 / 102 |

Private-buffer latency decreases by 33.7%; scalar latency decreases by 26.7%.
Release synthesis WNS is +8.740 ns and +8.129 ns respectively. The private-buffer
case still takes 29.6% more cycles than Vitis and retains a substantial FF gap.
Neither case establishes overall Vitis parity. These are normalized-wrapper
post-synthesis measurements, not post-route timing closure.

The final compiler reproduces both measured RTL files byte-for-byte. The
[evidence archive](baselines/u250_100mhz_backedge_20260921/README.md) contains
unchanged copies of the before/after measurements, regression and identity
audits, per-task diagnostic reports, and the prior narrow-IR verification error.

## Remaining latency gap

The private-buffer producer and consumer inner compute loops now use one cycle
per iteration, matching the corresponding Vitis reported loop iteration count.
Generated Light-HLS FSM analysis changes `load_slow` from 418 to 290 cycles and
`store_slow` from 338 to 210; the private buffer stays at 82. For diagnosis,
Vitis HLS's child reports list 225, 161, and 69 cycles respectively. Those child
figures are pre-RTL reports/un-stalled FSM counts, not independently measured
end-to-end transaction latencies.

The next concrete targets are external/FIFO response-to-initial-PHI forwarding
and outer-loop increment/branch transitions. Current outer iterations still
separate memory response capture, inner-loop initialization, and some exit tests.
Any fusion must preserve request/pop edges, live-outs, and clock-enable stalls.
This checkpoint does not change FIFO read latency, remove live-out registers,
or implement general cross-block bypassing, SDC scheduling, or RL search.

## Reproduction and artifacts

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-backedge-regressions --keep-going
python3 Light_HLS/Tests/OrdinaryBackedge/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-backedge-tests
```

Use the private-buffer comparison command in the
[FIFO checkpoint](FIFO_EVENT_PRESERVATION_20260921.md) and the existing scalar
FairComparison command. Sources, testbenches, normalized synthesis boundary,
Vitis/Vivado 2025.2, U250 `xcu250-figd2104-2L-e`, and 100 MHz are unchanged.
No native-boundary metric is substituted for normalized-wrapper synthesis.

Raw root: `/tmp/lighthls-backedge-qor-20260921.CGzauP/`. Final executable:
`Light_HLS_Top-release`; compiler SHA256:
`8db9a9f74bb67e570154bc242a0ec9d5e0cdbe8023b64fe5bac9728c2ae6e5c8`.
The unchanged stream-header SHA256 is
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.
The final measurements are under `u250-release/`; `u250-candidate/` and
`u250-final/` are earlier development attempts and are not the release archive.

The exploratory masked `input[(i+j)&15]` test hit an existing conservative
depth-inference rejection (23 inferred versus 16 declared). The committed
load-dependent test uses directly bounded `input[j]`; masked-depth precision
is not claimed fixed here.
