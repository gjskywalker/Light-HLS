# Ordinary-loop bank analysis optimization

This checkpoint closes the first remaining priority in the
[September 14 boundary optimization](RTL_BOUNDARY_OPTIMIZATION_20260914.md).
It improves partition-bank facts for ordinary loops; it does not replace the
scheduler or binder, change benchmark pragmas, or implement SDC/RL.

## Implementation

`HI_ArrayPartitionAnalysis` now obtains the enclosing ordinary loop when an
access is outside a pipeline region. For a single-level loop with a proven
constant trip count, it enumerates object-relative element addresses with
LLVM `APInt`, retaining the original integer widths. Supported expressions
include constants, affine recurrences belonging to that loop, addition,
multiplication, integer casts, and closed integer bitwise/shift operations.
The latter cover sign extension lowered to arithmetic shift by strength
reduction. Unsigned wrap and signed extension are not approximated by an
unbounded mathematical affine expression.

An `inttoptr` expression must contain exactly one additive recovered memory
object base. Arbitrary base multiplication, truncation, runtime values, and
unknown pointer provenance are not treated as zero. GEP traversal follows the
backend's existing flat element-address model, rejects unsupported aggregate
forms, and requires matching pointer/index widths. Pointer-width-changing
casts are conservative. The analysis does not rewrite LLVM IR.

The proof is limited to at most 1,000,000 iterations, scalar widths up to 64
bits, and bounded expression recursion. Every enumerated address must lie
inside the declared memory extent. Nested ordinary loops, unknown or excessive
trip counts, runtime-dependent offsets, unsupported expressions, and unproven
in-bounds addresses retain all-bank coverage. Existing pipeline analysis stays
on its previous path.

The resulting set is proven coverage, not necessarily a minimal bank set for
conditional accesses: all possible iterations are included even if a branch
skips the access. A proven set containing several banks still needs dynamic
bank selection and reserves every candidate bank. It does not imply that
several accesses with overlapping sets can execute together.

The existing scheduler and resource binder already reserve ports per bank.
Supplying narrower proven sets allows independent banks to issue concurrently;
same-bank requests still serialize, and potential RAW/WAR/WAW aliases retain
their request-edge constraints. Eligible one-cycle reads can then use the
existing response-ready forwarding path without new scheduling rules.

## Reports and diagnostics

Detailed `binding.json` memory accesses now include `is_write`,
`bank_mapping_exact`, `bank_analysis_source`, `bank_analysis_iterations`, and
`bank_analysis_reason`. The human-readable binding report adds a Memory Bank
Analysis table. The iteration field is the analysis coverage bound, not a
latency measurement; it can remain populated when expression analysis falls
back. A single bank needs no enumeration.

The top-level driver also registers LLVM's fatal-signal stack trace handler.
This improves evidence if the previously observed legacy estimator assertion
recurs. It is diagnostic instrumentation, not a fix for that assertion or the
previous intermittent linker failure.

## Correctness validation

The final immutable compiler passes all 11 local regression suites and the
independent 18-case App RTL smoke test. The capability matrix remains a separate
generation/syntax and expected-rejection check, not functional verification
for every App design.

The new [ordinary-bank suite](../Light_HLS/Tests/OrdinaryBankAnalysis/README.md)
adds ten fixtures at one- and two-cycle memory read latencies: 1,280 calls
without reset and 9,664 written-output checks, plus full input-memory and
request-count checks. It covers cyclic/block parallelism, genuine bank
conflicts, dynamic addresses/trip counts, zero iterations, truncation,
arithmetic sign extension, descending induction, mixed read/write hazards,
and conservative nested-loop handling. Scalar input capture is checked by
changing ports immediately after start acceptance. Bank proof metadata and
per-bank schedule occupancy are asserted before RTL simulation.

The existing local-partition App regression now also requires four proven
static read banks and simultaneous read requests. All new source comments
and diagnostic strings are English.

## U250 measurement

The unchanged `backend_array_partition_local` App and its common self-checking
testbench were rerun with Vitis/Vivado 2025.2, U250 `xcu250-figd2104-2L-e`, and
10 ns. Both RTL implementations pass simulation and the normalized interface
contract. Resources below use the same normalized-wrapper Vivado OOC flow,
not the pre-backend estimator.

| Metric | Previous Light-HLS | Current Light-HLS | Vitis HLS |
| --- | ---: | ---: | ---: |
| Testbench-observed cycles | 59 | 43 | 42 |
| LUT | 596 | 428 | 242 |
| FF | 310 | 184 | 152 |
| DSP / BRAM tiles | 0 / 0 | 0 / 0 | 0 / 0 |

Cycles decrease by 27.1%, LUT by 28.2%, and FF by 40.6%. Current Light-HLS
synthesis WNS is +8.664 ns; Vitis is +9.003 ns. The strict no-cycle/resource-
regressions check passes for this freshly measured case. The read requests
previously occupied cycles 0, 1, 2, and 3; all four now issue in cycle 0.
Forwarding also removes the previous unnecessary read-result staging.

The full set of 20 previously measured designs was regenerated: 19 RTL files
are byte-identical to the September 14 measurements, and only the local case
changes. Those 19 cases were not resynthesized and are not relabeled as new
measurements. Their old compiler fingerprints and archives remain untouched.
The cross-version identity audit intentionally reports an overall mismatch
for the optimized local case. A separate fresh-measurement identity audit
passes for the new local RTL. See the
[checkpoint archive](baselines/u250_100mhz_banks_20260918/README.md).

Cycle numbers retain the existing testbench's completion convention; they are
not renamed as final-memory-commit latency. This is not post-route timing
closure, formal equivalence for all inputs, or universal Vitis parity. Even
this improved local case still uses more LUT and FF than Vitis.

## Reproduction

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/lighthls-bank-regressions --keep-going
python3 Light_HLS/Tests/FairComparison/run_u250_fair_comparison.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --source Light_HLS/App/backend_array_partition/backend_array_partition_local.cpp \
  --top backend_array_partition_local \
  --testbench Light_HLS/App/backend_array_partition/backend_array_partition_local_tb.v \
  --testbench-top backend_array_partition_local_tb \
  --clock-period-ns 10 --output /tmp/lighthls-bank-u250
```

Use fresh output directories. Raw runs, development failures, and before/final
compiler snapshots are retained under
`/tmp/lighthls-bank-qor-20260918.ZR3t1F/`. Final compiler SHA256:
`fbe1e73f68f671f040aa636ac496511d4ce567ebeb231ef3550be870307de8cd`.

## Next priorities

1. Explain the remaining measured LUT/FF gap through generated storage,
   address-selection muxes, and controller structure. Calibrate cost choices
   against synthesized FF/LUTRAM rather than structural bit counts alone.
2. Extend bank proofs to bounded nested ordinary loops only with explicit
   multi-induction coverage and new alias/event regressions. Do not relax
   the current conservative fallback merely to lower a resource estimate.
3. Keep preserving crash artifacts and stack traces until the intermittent
   legacy-analysis assertion has a reproducible root cause.
4. Reconsider SDC or RL only after the relevant event semantics and measured
   cost models are stable. The existing seven pipeline App II results are
   unchanged by this checkpoint.
