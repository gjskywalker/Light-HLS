# Floating-point backend: implemented scope and validation

## Status

This is a functional IEEE binary16/binary32/binary64 backend increment, **not full
floating-point support** and not a timing-closed floating-point implementation.
Integer support remains available. The shared backend is used by both
`Light_HLS_Top` and `Light_HLS_DAPO`.

Implemented operations:

- FP16/FP32/FP64 scalar arguments, returns, constants, PHIs, selects, freezes,
  same-width bitcasts, local arrays, and `ap_memory` loads/stores.
- Add, subtract, multiply, divide, negate, all LLVM scalar `fcmp` predicates,
  the strict fused `llvm.fma` intrinsic, `llvm.sqrt`, `llvm.fabs`, and
  `llvm.copysign`.
- Conversion between FP16/FP32/FP64 and signed/unsigned integer conversion (integer widths
  2 through 64). FP-to-integer inputs must be in LLVM's defined range.
- Arithmetic rounds to nearest, ties to even; FP-to-integer truncates toward
  zero. Subnormals, signed zero, infinities, and NaNs are handled. NaN payload
  identity and an externally observable floating-point exception environment
  are not promised.

Not yet implemented or qualified:

- BF16, x86 extended precision, FP128, vector FP, constrained FP/dynamic
  rounding modes, exception-status interfaces, `frem`, and general
  math-library calls such as `exp`, `log`, or trigonometric functions.
- Internally pipelined arithmetic IP with initiation interval one for divide
  or square root. The current iterative units enforce a resource MII instead.
- Whole-design calibrated FP resource estimation and automatic timing closure.
  Array-partition/FP combinations still need a dedicated regression.

Floating-point loop PIPELINE and DATAFLOW now use typed context/recurrence
storage and scalar transaction queues. Existing backend restrictions still
apply: supported static loop shapes, no nested pipeline directives, validated
memory/FIFO channels and rates. Iterative operators freeze synchronously with
their task on backpressure. Arithmetic clocks are never combinationally gated.
Signature-checked libm sqrt/fabs/copysign declarations with exact scalar signatures are
lowered consistently in both drivers; user-defined functions are not replaced.
There is no errno/fenv side channel. This is not general libm support.

Arithmetic RTL uses the vendored Berkeley HardFloat Release 1 sources and
RISC-V specialization; see [provenance and license](HardFloat/PROVENANCE.md).
The generated `top.v` embeds its required modules and includes; downstream
simulators and Vivado do not need a separate HardFloat source file list.
The compiler currently locates the vendored RTL through its build-time source
directory, so retain `Light_HLS/RTL/HardFloat/source` when using the executable.

## Scheduling and estimator contract

Most FP operators are combinational and their results are registered on the
issue edge: scheduling latency is one cycle. This does **not** imply that the
datapath meets the requested clock. Division and square root use the iterative HardFloat core,
a dedicated instance for each iterative instruction, and fixed completion
slots of 16 cycles (FP16), 29 cycles (FP32), or 58 cycles (FP64), including result retention.
Special-value division follows the same schedule, avoiding input-dependent
cycle variation and uninitialized internal special-case state.

`HI_FloatingPoint.h` is the shared operator-latency/type contract. The RL
estimator runs the same scheduling path used for emitted RTL. Floating-point
reward reports require a valid scheduled envelope: neither `heuristic` mode
nor a `hybrid` heuristic fallback is accepted for floating-point rewards.
Reports and candidate records explicitly mark FP timing and resource estimates
as not whole-design calibrated. The pre-backend resource heuristic is retained for comparison,
but is not a measured HardFloat implementation resource result.
In particular, native half arithmetic uses the legacy FP32 operator-cost lookup
as an explicitly reported uncharacterized surrogate, not an FP16 resource
calibration. Half storage/register widths are 16 bits; binding distinguishes
half, float, and double types rather than grouping all non-double operators.

`RTL/floating_resources_u250.cfg` now supplies 30 measured U250/100 MHz isolated
operator costs (three formats times eight operations, plus six conversions).
The separate `floating_operator_resources` field sums **bound instances**, not
IR instruction counts, and aggregates DATAFLOW child tasks. Missing keys,
unavailable binding, other boards/clocks, and incompatible model versions are
explicitly reported; absent entries are not assumed to cost zero.
Integer conversion and comparison operator costs are not characterized yet.
`complete` means complete lookup coverage of the FP units in that report, not
whole-design accuracy or timing closure. Controller, mux, memory, pipeline
context storage, clock-enable overhead, constant folding, and cross-operator
synthesis optimization are outside this additive model. Do not add its FF
count blindly to the legacy resource estimate.

Scheduled estimate-only runs now execute pipeline storage analysis and resource
binding, but still do not generate RTL or run synthesis per RL action. The
resource-model hash is frozen during a search and the costs are retained in
candidate/cache records. Reward remains schedule-derived cycles, not resources.

The source language and target determine whether `_Float16` expressions become
native half arithmetic or FP32 arithmetic with conversions. The current default
Clang/x86 preparation uses the latter for ordinary half arithmetic. Light-HLS
preserves that IR semantics; it does not silently replace promoted arithmetic
with native half operations. Native half IR is separately supported and tested.
See the [Clang floating-point documentation](https://clang.llvm.org/docs/UsersManual.html).

Post-search legalization consistently lowers bounded constant-array `memcpy`
initializers and expands optional `llvm.fmuladd` into unfused multiply/add for
both baseline and candidates. Strict `llvm.fma` is never split. These steps and
their counts are recorded in `dapo_ir_audit.json`; the input candidate is not
overwritten.

Additional correctness repairs from the CV0 audit:

- Missing array geometry no longer causes estimator division by zero.
- Unused pointer arguments no longer require an invented memory data width.
- Both legacy duplicate-instruction passes use LLVM's exact instruction
  equivalence, preserving comparison predicates, cast types, flags, and exact
  constant semantics. Opcode-plus-operands previously merged `<` with `==`.

## Local validation and commands

Core implementation changes are in:

- `Implementations/HI_BackendCommon/HI_FloatingPoint.h` and
  `HI_ConstantMemoryLowering.h` (new shared contracts/legalization).
- `HI_BackendCommon/HI_BackendContext.h`, `HI_BackendValidation`,
  `HI_MemoryObjectAnalysis`, `HI_OperationScheduling`, `HI_ResourceBinding`,
  `HI_RTLGeneration` (including new `HI_FloatingPointRTL.{h,cc}` and CMake entry).
- `HI_WithDirectiveTimingResourceEvaluation` type/width handling,
  `HI_DuplicateInstRm`, `HI_HLSDuplicateInstRm`, and `HI_VarWidthReduce`
  (robustness, half-width handling, and semantic-equivalence repairs).
- `Tests/Light_HLS_Top/Light_HLS_DAPO.cc`, `Light_HLS_Top.cc`, and `Build.sh`
  (both drivers are built together).
- `Models/RLModels/dapo_ii_pass_order.py` and its configuration documentation;
  both experiment controllers now default to the normal build's estimator.
- `App/backend_floating`, `RTL/HardFloat`, and the CV typed verifier/audit tools.

Generated files remain under ignored output directories. Local-only unit and
RTL regression helpers retain the pre-existing ignore policy.

The current development and default RL build is `Tests/Light_HLS_Top/build`.
To rebuild the normal executable from the repository root:

```bash
./Light_HLS/Tests/Light_HLS_Top/Build.sh \
  -DLLVM_DIR=/usr/local/lib/cmake/llvm \
  -DPolly_DIR=/usr/local/lib/cmake/polly \
  -DHI_BUILD_LEARNER_LIBS=ON -DHI_USE_LLD=OFF

./Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  Light_HLS/App/backend_floating/backend_floating.cpp backend_floating \
  --board u250 --freq 100MHz --output runs/floating_demo
```

`Tests/DAPOII/verify_floating.py` is a local-only regression helper, following
the repository's existing ignore policy. It checks native C++ oracle bits,
Icarus RTL bits, repeated transactions, special values, and scheduled cycle
bounds. `--classic-binary` checks the production source driver as well.
The CV verifier additionally handles typed FP16/FP32/FP64 arrays, native candidate
IR, ASan/UBSan fixtures, and NaN-aware bitwise output checking.

Validation artifacts are under ignored `runs/` directories:

- `cv_floating_canonical_ir`: 1,893 FP32 and 2,162 FP64 vectors, zero failures.
- `cv_floating_canonical_classic`: the same 4,055 vectors through the production
  source driver, zero functional failures after the duplicate-pass repair.
- `cv_floating_release_rl128`: three independent PPO searches using the normal
  default build, seed 0, 128
  actions/design, frozen GNN, U250/100 MHz, four fixture cases per variant.
  This is a bounded integration experiment, not convergence/generalization.
- `cv_scalar_width_regression` and `cv_bool_fixture_final`: the five previously
  unsupported char/unsigned-char/bool interfaces now pass baseline and selected
  RTL verification. The bool stencil requires an audited legal shift/bank
  fixture; its initial generic fixture was rejected by UBSan, not hidden.
- `cv_floating_vivado/{float,double}`: out-of-context synthesis of the scalar
  arithmetic test kernels. FP32: 4,226 LUT / 704 FF / 4 DSP, WNS -2.221 ns.
  FP64: 9,032 LUT / 1,362 FF / 18 DSP, WNS -5.001 ns. Both synthesize, but
  **neither meets the 100 MHz constraint**. These are not routed Fmax results.

FP16 extension validation (2026-11-11):

- `fp16_source_pinned` and `fp16_native_pinned`: 10,531 vectors each, no
  numerical or scheduled-cycle-envelope failures. Independent software LLVM
  APFloat oracle; source-promoted and native half arithmetic are checked
  separately. Native half IR is emitted with `-mavx512fp16` but that target code
  is never executed as the host reference. Tests include strict half FMA,
  explicitly promoted FP32 FMA, subnormals, signed zeros, NaNs, infinities,
  and repeated transactions. NaN payload identity is not required.
- `fp16_classic_v2`: 2,431 vectors through the normal source driver, no
  functional failures. The first run exposed a missing half-width case in
  `HI_VarWidthReduce`; that assertion is fixed.
- `fp16_conversions_v2`: 3,224 vectors covering all six directed format
  conversions, no numerical or cycle-envelope failures. Includes narrowing
  ties, underflow, overflow, special values, and random bit patterns.
- `fp16_extension_regression_{ir,classic}`: 4,055 FP32/FP64 vectors per driver,
  all passing after the half-format extension.
- `fp16_memory`: 16-bit external/local memory regression and one 128-step
  PPO/GNN search (seed 0). Baseline 161 cycles, selected candidate 19 cycles;
  source, native candidate IR, and RTL outputs match on four fixtures each.
  Measured cycles equal the scheduled estimates for both variants. This is
  a bounded integration test, not evidence of policy convergence.
  The normal source driver's independently optimized memory RTL also passes
  four fixtures (96 measured cycles); it is not the same IR as the DAPO baseline.
- `fp16_vivado_native`: U250/100 MHz OOC synthesis succeeds, 4,278 LUT / 598 FF /
  4 DSP, WNS -2.342 ns. This test kernel includes FP32 FMA and conversions as
  well as half arithmetic; these are whole-kernel figures, not a pure FP16
  operator cost or a fair cross-format resource comparison. Timing is not met.
- CV verifier unit tests: 20 passed; RL estimator-contract unit tests: 15 passed.

Large unpinned Icarus runs produced an internal assertion/segmentation fault
(`fp16_source_v1` retains the failure). CPU-affined replay and the two new full
runs completed with no mismatches. This is an execution-environment workaround,
not a claimed fix to Icarus. Reproduce the full tests with, for example:

```bash
taskset -c 8 python3 Light_HLS/Tests/DAPOII/verify_floating.py \
  --binary Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_DAPO \
  --formats 16 --random-cases 1000 --native-half \
  --output runs/half_native_new
```

Choose an available CPU on other hosts. The regression helper and APFloat
oracle remain local-only under the existing test-file ignore policy.

| CV0 design | Baseline cycles | RL-selected cycles | RTL validation |
|---|---:|---:|---|
| random10 | 11000 | 11000 | Four cases match exactly for each variant |
| random25 | 1024 | 448 | Four cases match exactly for each variant |
| random9 | 3649 | 1153 | Four cases match exactly for each variant |

The original 80-design overnight artifacts remain unchanged. A new baseline
audit accepts 71/80 designs in hybrid mode (FP heuristic fallback remains
forbidden), compared with 32/80 in the original run. Strict scheduled mode
accepts 70/80. Of the 39 FP designs, 34 now have valid scheduled estimates;
the other five fail memory/depth or static-loop-bound checks rather than the
old blanket floating-point rejection. Baseline estimate
acceptance is not a claim of RTL correctness: pointer PHIs, global-memory
provenance, unknown loop bounds, declared-depth violations, and source UB are
still reported separately. A source-UB failure invalidates the functional
verification, even when its IR has an accepted estimate. Unsupported backend
operations are not silently assigned a valid reward.

The next required engineering stages are timing-characterized FP operators,
whole-design resource-model validation, additional math
operations, and a wider synthesis/cosimulation regression. Until those pass,
do not label this implementation as fully supporting all floating-point designs
or use its cycle counts alone to claim target-frequency QoR parity with Vitis.

## Math, pipeline, DATAFLOW and calibration follow-up

Local artifacts: `runs/floating_followup_20261111/RESULTS.md` and `WORK_LOG.md`.
These ignored run reports are the detailed review entry points on this machine.

- sqrt/fabs/copysign: 5,352 vectors for each of binary16/32/64 pass. Existing
  arithmetic regressions also pass after adding synchronous clock enables.
- Nine pipeline/recurrence format combinations pass four source/native-IR/RTL
  fixtures each, with exact scheduled-cycle agreement. Strict half FMA tests
  cover native half arithmetic separately from promoted source expressions.
- PIPO floating scalar transport passes four fixtures. FIFO tests pass four
  transactions and reset-abort in all formats, including thousands of cycles
  with an active iterative operator frozen by downstream backpressure.
- Thirty isolated U250 operator measurements are installed: eight arithmetic/
  sign operations per format and six precision conversions. Coverage is not
  whole-design calibration; missing cost keys are explicitly partial.
- A 128-step PPO/GNN smoke run selects a 265-cycle pipeline from a 497-cycle
  baseline. All four independent RTL fixtures confirm 265 cycles. LUT/FF rise
  from 2557/398 to 3906/725; both use two DSPs and fail the 100 MHz OOC timing
  constraint (WNS -2.049/-2.020 ns). Do not call this overall QoR parity.
- The FP32 FIFO test synthesizes to 1921 LUT / 473 FF / zero DSP, WNS +0.561 ns
  at 100 MHz OOC. This is not a post-route frequency measurement.

The GNN extractor now skips external function declarations before querying loop
analyses and recognizes half-width features. DAPO source preparation still
rejects DATAFLOW; the tested DATAFLOW path uses the standard driver. No full
80-design cross-validation rerun was performed in this follow-up.
