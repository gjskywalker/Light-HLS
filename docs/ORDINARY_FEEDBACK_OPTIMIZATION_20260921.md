# Ordinary-loop feedback storage optimization

This checkpoint follows the [FIFO correctness work](FIFO_EVENT_PRESERVATION_20260921.md)
and targets the duplicated induction/update storage identified in the
[local-memory payload checkpoint](LOCAL_MEMORY_PAYLOAD_OPTIMIZATION_20260918.md).

## Implementation and safety boundary

`HI_OperationScheduling` can make a constant-step integer add/subtract update
combinational when it feeds its own held PHI on a single-block ordinary loop's
self-backedge. The PHI remains registered. This removes the next-value register,
not the old induction value needed by earlier or same-edge memory requests.

Selection requires two PHI inputs, a direct matching self-edge, a constant
step, no other PHI users, and no non-debug users outside the block. PIPELINE
regions are excluded. Existing operator characterization and the 70%-of-period
combinational-chain budget still apply; a later timing cut removes the feedback
candidate too. Cast-mediated, variable-step, multiblock, and cross-block live-out
updates retain the existing storage. A supported inner ordinary loop may qualify
even when its surrounding ordinary loop does not.

The existing binder and emitter already exclude combinational SSA values from
register allocation and sample PHI inputs on CFG transitions. Their memory
request timing, CE/WE, FIFO event ordering, and shared DATAFLOW clock enable are
unchanged. Post-RTL validation independently checks the recorded feedback shape,
users, self-branch, and absence of pipeline membership. Scheduling JSON/RPT now
lists each feedback wire and its held PHI. Reported IR bits are not physical FF
savings; synthesis can already trim or merge some bits.

## Width-reduction correctness fix

The new wraparound tests exposed invalid IR in the previous compiler too:
`HI_VarWidthReduce` could plug an i9 update into an i32 PHI, or replace an i32
function return with an i9 return. The frozen pre-change compiler reproduces
the failure; `opt -passes=verify` rejects the intermediate PM2/PM4 modules.

The pass now restores each PHI operand to the PHI's type on the corresponding
incoming edge and each return operand to the declared function return type.
Conversions use the pass's existing signedness decision. This preserves the
interface type while retaining narrowed arithmetic. Both unsigned and signed
return oracles pass; this is not a proof of all legacy width-reduction rules.

## Validation

The final compiler passes all 14 local regression suites and all 18 independent
App smoke checks. The new [OrdinaryFeedback suite](../Light_HLS/Tests/OrdinaryFeedback/README.md)
executes 17 configurations and 1,088 calls without reset, including exact
memory-event counts and return-value checks. Existing FIFO backpressure,
private RAM freeze, PIPO, storage binding, and pipeline tests remain enabled.

An RTL identity audit of the 20 historical comparison sources finds 15 unchanged
files and five changes: scalar, complete-partition read/write, local cyclic
array, FIFO DATAFLOW, and PIPO DATAFLOW. Those five designs, plus the restored
private-buffer FIFO case, are remeasured with the final compiler. Old comparison
fingerprints are not relabeled. The other 15 designs are not freshly synthesized.

## Measured U250 results

All six fresh comparisons pass functional and normalized-interface checks.
The strict checkpoint audit passes: LUT and FF both decrease in every case,
cycles and DSP/BRAM use are unchanged, and synthesis WNS stays positive.

| Case | Light LUT before -> after | Light FF before -> after | Vitis LUT / FF | Cycles Light / Vitis |
| --- | ---: | ---: | ---: | ---: |
| Complete partition read/write | 207 -> 203 | 47 -> 42 | 230 / 7 | 15 / 5 |
| Local cyclic array | 297 -> 278 | 181 -> 153 | 242 / 152 | 43 / 42 |
| FIFO DATAFLOW | 214 -> 204 | 172 -> 139 | 164 / 54 | 136 / 132 |
| PIPO DATAFLOW | 375 -> 360 | 279 -> 222 | 275 / 126 | 393 / 388 |
| Scalar | 157 -> 156 | 269 -> 266 | 106 / 102 | 15 / 4 |
| Private-buffer FIFO | 354 -> 345 | 522 -> 510 | 324 / 316 | 760 / 389 |

The local-array FF reduction is 15.5%; FIFO/PIPO DATAFLOW FF reductions are
19.2%/20.4%. Local-array netlist inspection confirms 128 RAM output FFs,
20 held induction FFs, and five FSM/status FFs: the redundant update registers
are gone without changing the RAM output registers. Its 278 LUTs consist of
150 logic LUTs and the unchanged 128 distributed-memory LUTs. Vitis still wins
that case's total LUT count and latency; this is not overall parity.

The final compiler reproduces all six measured RTL files byte-for-byte.
See the [compact evidence archive](baselines/u250_100mhz_feedback_20260921/README.md)
for before/after metrics, synthesis timing, regression reports, fingerprints,
identity audits, and the local-array primitive inventory. The FIFO/PIPO native
interfaces are not identical; the existing common normalization contract is
used and both sides pass its checks. Native synthesis metrics are not substituted
for the normalized-wrapper results above.

## Reproduction

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-feedback-regressions --keep-going
python3 Light_HLS/Tests/OrdinaryFeedback/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-feedback-only
```

Use the existing FairComparison drivers for `array_partition/{local,complete_rw}`,
`dataflow/{fifo_stream,pipo_array}`, and the scalar example. The private-buffer
comparison uses the command in the FIFO checkpoint notes. All new QoR results
use unchanged sources/testbenches, Vitis/Vivado 2025.2, U250
`xcu250-figd2104-2L-e`, 100 MHz, and the common normalized-wrapper Vivado OOC flow.
They are post-synthesis results, not post-route timing closure or universal
Vitis parity. Completion cycles retain each existing testbench's convention.

Raw artifacts and frozen before/final executables are under
`/tmp/lighthls-feedback-qor-20260921.5am2BK/`. Final compiler SHA256:
`068c78311810d08048ca0c49f867186cf628e426bcecc7852a72f75b6444daff`.
The runtime stream header is unchanged from the FIFO checkpoint (SHA256
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`).

## Remaining work

The optimization deliberately does not shorten ordinary loop initiation
intervals or relax memory/FIFO dependencies. In the private-buffer case, generated
FSM analysis reports 418 cycles for `load_slow`, 82 for `private_buffer`, and 338
for `store_slow` before channel stalls. The producer/consumer inner compute loops
still register their arithmetic recurrence and then update the PHI in another
state. Next priorities are these compute/backedge transitions and remaining
local-array address/control muxes, with memory-edge and backpressure correctness
retained. General or cast-mediated recurrence elimination, especially with
live-outs, needs a separate type/edge proof before extending this rule.
Replacing the entire scheduler with SDC or starting RL is not required for these
concrete improvements.

Exploratory tests also confirm existing unsupported boundaries: source pragma
discovery before preprocessing, unbounded dynamic-depth inference, generic
`llvm.umin`/`llvm.umax` lowering, and a legacy timing lookup assertion for a
0.1 ns period outside the characterized library. The checked regression uses
bounded source indices and the characterized 3 ns minimum; these limitations
are not claimed fixed or counted as successful unsupported tests.
