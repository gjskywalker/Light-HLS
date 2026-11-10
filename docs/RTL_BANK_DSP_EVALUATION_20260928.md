# Bank relations, arithmetic mapping, and evaluation hardening

## Scope and provenance

This checkpoint implements steps 1–3: relational bank/port analysis, integer
DSP mapping, and stronger evaluation. It does not start RL training or claim
general physical timing closure. The previous checkpoint in
`baselines/u250_four_work_20260928` is unchanged.

The final compiler SHA256 is
`53d44a4998aee81d29d0782181aaae4a76bda53d682499342f21fe2f8e616067`.
Raw runs, frozen before/after executables, RTL, DCPs, and logs are under
`/tmp/lighthls-next3-20260928.t1orVg`. Compact evidence is archived under
`baselines/u250_bank_dsp_20260928`. Measurements made before the final
report-format changes are retained with their original compiler fingerprints;
release RTL identity is checked separately, not asserted by rewriting them.

## 1. Relational bank/port analysis

`HI_ArrayPartitionAnalysis` now retains an exact bank trace over up to 4096
logical iterations for closed, bounded addresses. The APInt/SCEV evaluator
preserves integer widths, truncation, extensions, masks and XOR. Memory bounds
must be known and every evaluated address must be inside the object. Unknown,
runtime-dependent, or excessive traces retain conservative bank coverage.

`HI_BackendCommon/HI_BankConflict.cc` supplies one collision predicate to
scheduling, binding and post-binding validation. Requests at starts `a`, `b`
can coincide only when `i * II + a == j * II + b`; the predicate checks actual
banks at both valid iteration indices. Complementary banks in one iteration
are not automatically complementary across pipeline stages.

Scheduling uses the aggregate bank capacity as an initial bound only for
fully traced memory groups, then assigns static ports using the exact overlap
test. Failed bounded placement retries at a larger II instead of looping
forever. Binding repeats the same proof; validation rejects collisions.
Existing RAW/WAR/WAW dependence ordering is not weakened. This is bounded
relational analysis and greedy port assignment, not complete symbolic alias
analysis or optimal graph coloring.

The `tile[i] + tile[i ^ 1]` cyclic-factor-two fixture now has II=1 in both
loops. The second loop previously had II=2. The latency-two memory variant
also passes at 250 MHz. An independent enumerated-time oracle checks 20,000
random bank traces, stage offsets and IIs, in addition to unknown and ordinary
access fallback cases.

## 2. Integer DSP mapping

Inspection of the old Vivado checkpoint found two cascaded DSP48E2s for the
masked multiply; the second multiplier's inputs had become constant zero.
This was a width/inference issue, not two required independent multipliers.

`HI_BackendCommon/HI_ArithmeticMapping.cc` uses LLVM known bits to remove only
proven leading zeroes from unsigned multiplication inputs. RTL generation
emits explicit operand-width casts. Binding uses the maximum required width
across every operation sharing a multiplier, never just the representative.
Result widths and modular overflow, operation schedules, latencies, enables
and stalls are unchanged. Negative/full-width values are not narrowed based
on an unsigned-range guess. There is no forced DSP primitive or blanket
`use_dsp` attribute.

The representative masked multiply now uses one DSP, matching Vitis. Tests
include 2048 arithmetic transactions with high bits, overflow, and shared
narrow/full-width operations at 100 and 250 MHz, plus the existing queued and
stalled DATAFLOW pressure tests. This does not establish optimal MAC fusion
or optimal DSP pipeline-register packing for arbitrary expressions.

## 3. Evaluation contract

The new common flow is `vivado_ooc_normalized_v3_pre_synth_io20`. Both
generators receive the same U250 part, normalized interface and constraints.
Clock and IO XDC are loaded before synthesis. Maximum input/output delays are
20% of the period; minimum delays are zero. The standalone `--synthesis`
adapter uses the corresponding native-interface contract. These are explicit
kernel-boundary assumptions, not measured board delays.

Conditional Tcl constraints use `read_xdc -unmanaged`, as described in
[AMD's constraint scripting documentation](https://docs.amd.com/r/2024.2-English/ug894-vivado-tcl-scripting/XDC-Constraints-read_xdc-versus-source).
An initial managed-XDC attempt was rejected by Vivado and is not part of the
accepted measurements. Old and new compiler baselines were both rerun under
the new flow; the archiver rejects mixing legacy and new flow IDs.

`--implement` adds optimization, placement, physical optimization and routing
for both normalized designs. Synthesis and routed DCPs, utilization, timing,
route status and DRC reports remain separate. Setup, hold, routing errors and
timing coverage are checked. Failed hold constraints do not become successful
reward samples. Period minus WNS includes IO budgets and is not measured Fmax.

The sustained-traffic harness checks 32 held-start offers with changing scalar
arguments, immutable RAM input and one-cycle memory responses against native
C. It records ready acknowledgements, done edges and final commits separately.
It discards four warmup intervals for steady throughput, retains every raw
event, checks all 8192 outputs and checks drain behavior. XSim is used for both
generators without modifying their generated RTL.

## Measured synthesis and cycle results

Cycles below are accepted-launch to final-memory-commit cycles, not merely
`ap_done`. Before/after rows use the same new constraints. Resources are from
the normalized post-synthesis boundary, not HLS estimates or routed reports.

| Case | Metric | Before | After | Vitis |
|---|---|---:|---:|---:|
| Banked BRAM, 250 MHz | Cycles | 771 | 515 | 515 |
| Banked BRAM, 250 MHz | LUT | 182 | 158 | 112 |
| Banked BRAM, 250 MHz | FF | 133 | 91 | 78 |
| Banked BRAM, 250 MHz | BRAM tiles | 1 | 1 | 1 |
| DSP map, 100 MHz | Cycles | 259 | 259 | 259 |
| DSP map, 100 MHz | DSP | 2 | 1 | 1 |
| DSP map, 100 MHz | LUT | 52 | 44 | 15 |
| DSP map, 100 MHz | FF | 32 | 32 | 46 |

Additional new-flow measurements: DSP map at 200 MHz uses 1 DSP, 91 LUT and
57 FF, with 260 final-commit cycles versus Vitis's 1 DSP, 16 LUT, 46 FF and
259 cycles. DSP DATAFLOW at 250 MHz uses 1 DSP, 219 LUT and 147 FF versus
Vitis's 1 DSP, 168 LUT and 79 FF. Higher-frequency control/register overhead
remains significant and is not hidden by the DSP-count improvement.

For the 250 MHz DATAFLOW fixture, steady final-commit intervals are 261 cycles
for Light-HLS and 260 for Vitis: a 0.38% interval gap. Both produce the same
8192 C-reference outputs. The previous 100 MHz DATAFLOW checkpoint measures
260 cycles per transaction for both generators under the same traffic.

The constraint-sweep skill supplied a planning grid; only actual runs at
100, 200 and 250 MHz are measurements here. Its plan-only summary is not
evidence that every planned frequency was tested. No globally optimal
frequency is claimed.

## Validation and limitations

- All 27 selected local regression suites pass; the summary distinguishes
  completed suite count from expected suite count so partial runs cannot pass.
- The 18-case App smoke set passes. The bank latency-two and arithmetic
  boundary tests are included in the regression suite.
- The three representative routed comparisons are DSP map at 100 MHz, banked
  BRAM at 250 MHz, and DSP DATAFLOW at 250 MHz. Their full timing results,
  including failures, are retained in `implementation_audit.json`.
- OOC virtual IO is not a placed board shell. Banked BRAM has input hold
  slack of -0.080 ns on both generators. Its internal register paths pass:
  Light setup/hold = 2.355/0.053 ns, Vitis = 2.520/0.053 ns. The failing full
  boundary result is not waived by that diagnostic. DSP map also has a
  Vitis input hold failure; DATAFLOW has a Light coefficient-to-queue input
  hold failure. DATAFLOW full setup/hold is 1.756/-0.135 ns for Light and
  1.695/-0.136 ns for Vitis; its internal register paths pass at
  1.756/0.041 ns and 2.351/0.041 ns respectively. A registered,
  physically constrained common shell or a
  justified external timing contract is needed before claiming full closure.
- An additional non-DATAFLOW held-start probe fails on `dsp_map`: the second
  transaction observes the previous coefficient at the scalar capture
  boundary. This is recorded as a failed probe, not folded into the passing
  DATAFLOW claim. Ordinary back-to-back handshake/scalar capture is the next
  correctness priority. Existing per-call tests passing does not establish
  support for this traffic pattern.

The cycle and DSP goals for these representative fixtures are achieved, but
general RTL resource parity, full physical closure and universal sustained
transaction support are not claimed.
