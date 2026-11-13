# Pipeline boundary and recurrence storage optimization

This checkpoint continues the September 11 response/storage optimization.
It changes backend scheduling, storage allocation, and RTL emission. It does
not change benchmark source pragmas, replace the scheduler with SDC, or
implement an RL policy.

## Implementation contracts

- Hardware address constants are recorded in the backend context without
  modifying LLVM IR. A `ptrtoint` is zero only for a proven recovered memory
  object root in the existing object-relative addressing model. Other integer
  instructions are folded only when all operands are known constants. PHIs,
  loads, side effects, and nonconstant pointers retain their original path.
- An empty function entry with an unconditional pipeline successor no longer
  consumes an FSM state. Pipeline initialization joins start acceptance only
  when all stored PHI initial values are constants. Scalar arguments are still
  latched at acceptance; runtime seeds initialize on a later edge. Pipelines
  reached through other control flow retain the existing initialization path.
- Retirement uses actual write edges. A final store can share the completion
  edge; an exported registered value must be ready before the edge that samples
  it. Memory read capture latency and recurrence/dependence II constraints are
  not reduced by this boundary optimization. Valid tokens and task enables
  continue to gate side effects under DATAFLOW backpressure.
- The independent storage pass can coalesce a retirement-only producer with
  its exact distance-one feedback PHI. Both must have identical integer type
  and one storage slot. They receive the same producer expression on the same
  edge, and only the PHI requires initialization. General SSA register sharing
  and multislotted recurrence coalescing are not implemented.
- Earlier retirement can shrink a controller ring to a prime size, forcing a
  short-lived value to retain an unnecessarily large divisor-sized buffer.
  Storage analysis also evaluates the next power-of-two ring. It expands only
  if payload-bit savings exceed additional iteration-index bits. This does not
  model synthesized FF versus LUTRAM cost or guarantee minimum FPGA area.
- DATAFLOW regular-access offsets and structural task latency reflect entry
  initialization and retirement choices; report estimates are not substituted
  for RTL simulation or Vivado measurements.

## Reports

`schedule.json` adds `hardware_constant_count`. Pipeline JSON/RPT includes
`initialized_at_entry`, `storage_alias_count`, and optional per-value `alias_of`.
`value_storage_bits` excludes aliased duplicate storage; the uncompacted count
still describes the logical full-ring alternative. These are structural
counts, not measured FPGA FF utilization. Binding accounts for the emitted
storage and controller separately from post-synthesis QoR.

## Validation

The ten-suite local regression driver and the independent 18-case App smoke
test cover the existing supported subset. The pipeline-storage suite retains
its seven configurations and 14,336 checked outputs, including repeated
transactions, ring wraparound, multicycle reads, dynamic banks, dual-port
forwarding, and depth-one FIFO backpressure.

Three new recurrence fixtures add 192 calls without reset and 3,328 read checks.
They vary scalar arguments immediately after start acceptance and verify
sampled values, recurrence results, request counts, and return-port stability
before completion. The constant-seed case must initialize at entry; the
runtime-seed case must retain the separate initialization edge. The
memory-seeded case additionally requires its preheader read to complete.

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/lighthls-boundary-regressions --keep-going
```

Working runs and immutable compiler snapshots are retained under
`/tmp/lighthls-boundary-qor-20260914.qyGycC/`.
One linker abort and two legacy-estimator assertions occurred during the work.
Passing retries are recorded without claiming these intermittent failures were
fixed; see [the preserved failure notes](../baselines/u250_100mhz_boundary_20260914/tool_failure_notes.md).

## Remaining priorities

1. Extend exact partition-bank analysis to ordinary loops, not only pipelined
   regions. In the unchanged local-partition App fixture, `local[4*i+0..3]`
   targets four distinct cyclic banks. Current `analyzeAccessBanks` passes a
   loop to its affine analyzer only for a pipeline region. The ordinary loop
   therefore reports all four banks for each read and serializes their request
   cycles as 0, 1, 2, and 3. First prove the trip range and bank residues, then
   reserve independent banks concurrently; keep conservative fallback for
   unknown ranges, overflow, and genuinely aliasing accesses. Validate this
   against the existing dynamic-bank and multicycle-memory tests.
2. Use measured FF/LUTRAM and mux costs to guide storage choices. Structural
   register reductions alone are not a complete FPGA area objective. Reduce
   scalar/control overhead while retaining same-edge memory ordering, input
   capture, and bounded-channel backpressure semantics. Distinguish return
   validity from the stronger current return-hold contract before sharing
   output and feedback storage.
3. Expand adversarial correctness coverage before broadening unsupported
   control-flow, memory, or pragma combinations.
4. Consider a stronger resource-constrained scheduler only with validated
   dependence edges, event timing, and calibrated implementation costs.
   At this checkpoint's 10 ns constraint, all seven pipeline App comparisons
   achieve the same loop II as Vitis. This does not prove the scheduler is
   optimal; it makes boundary control, storage, and the known ordinary-loop
   bank-analysis gap more specific immediate targets for this corpus.

This remains a supported-subset optimization checkpoint, not universal Vitis
parity, formal equivalence, or post-route timing closure.

## Completion-event interpretation

An additional event probe runs the unchanged simple pipeline App testbench
against both generated RTL implementations. The probe is
`Tests/FairComparison/pipeline_event_probe.v`; select `pipeline_event_probe`
as the Icarus top and include it with `backend_pipeline_tb.v` and all RTL files
for the selected generator. It observes the pre-NBA signals sampled by the
synchronous memory model and separately logs the rising edge of `ap_done`.

For this checkpoint's 10 ns simple-pipeline fixture:

| Event | Previous Light-HLS (ns) | Current Light-HLS (ns) | Vitis (ns) |
|---|---:|---:|---:|
| First start sampling edge | 45 | 45 | 45 |
| First read request sampled | 75 | 55 | 45 |
| First output write sampled | 85 | 65 | 55 |
| `ap_done` rises | 245 | 215 | 195 |
| Final output write sampled | 235 | 215 | 205 |

The existing common testbench reports 18 versus 16 cycles to observed done,
then waits another rising edge before checking memory contents. The final
memory-commit gap is one cycle, not two. Vitis starts reading on acceptance
and asserts done while its final write controls are presented for the next
memory edge. Light-HLS starts the request one edge later and registers done
on the final write edge. Both pass the same full-output check. The archived
cycle field is left unchanged for checkpoint comparability; it is not relabeled
as a memory-commit latency or intrinsic scheduler quality.
Relative to the previous checkpoint, start-to-done improves by three cycles:
two from earlier requests and one from removing the extra post-write retirement
edge. The memory-commit improvement itself is two cycles.

The recurrence RTL also retains a dedicated registered `ap_return` in
Light-HLS. In the measured Vitis recurrence RTL, `ap_return` is a combinational
sum. Thus some FF and cycle difference is attributable to output-hold semantics,
not recurrence dependence or II. This checkpoint preserves the existing
Light-HLS return-hold behavior and tests it across calls. Future interface-level
optimization must make validity/hold requirements explicit before removing
that register; resource reduction must not silently change the tested contract.

## Measured checkpoint

The [20-case archive](../baselines/u250_100mhz_boundary_20260914/README.md)
uses one compiler SHA256 and unchanged source/testbench fingerprints relative
to September 11. All comparisons pass the common functional and normalized
interface checks; all Light-HLS designs meet the 10 ns OOC synthesis constraint.
All 20 measured RTL inputs regenerate byte-for-byte from the final executable.

| Case | Previous cycles | Current cycles | Vitis cycles | Previous LUT/FF | Current LUT/FF | Vitis LUT/FF |
|---|---:|---:|---:|---:|---:|---:|
| Simple pipeline | 21 | 18 | 16 | 38 / 25 | 34 / 18 | 9 / 11 |
| Memory-II pipeline | 38 | 34 | 32 | 91 / 71 | 75 / 56 | 53 / 47 |
| Recurrence | 21 | 19 | 16 | 53 / 121 | 49 / 87 | 72 / 39 |
| Outer pipeline | 21 | 18 | 17 | 73 / 36 | 63 / 23 | 58 / 52 |
| Cyclic partition | 9 | 6 | 4 | 123 / 17 | 121 / 12 | 96 / 7 |
| Partitioned DATAFLOW | 44 | 38 | 34 | 477 / 353 | 481 / 339 | 223 / 175 |
| Nested partitioned DATAFLOW | 46 | 41 | 34 | 484 / 353 | 478 / 342 | 237 / 184 |

Across all 20 cases, thirteen improve without increasing any measured cycle
or resource metric, five have mixed tradeoffs, and two remain unchanged.
Eighteen reduce cycles; none increase cycles. The strict no-metric-regressions
gate intentionally does not pass. Complete read/write adds 1 LUT, local
partition adds 7 LUT, FIFO adds 4 FF, PIPO adds 6 FF, and partitioned DATAFLOW
adds 4 LUT while saving 14 FF. The archived audit retains each metric rather
than hiding these changes in an aggregate area score.

Scalar remains at 15 cycles versus Vitis's 4; local partition remains at
59 versus 42, with 596 LUT / 310 FF versus 242 LUT / 152 FF. The ordinary-loop
bank-analysis limitation described above is therefore a concrete next target.
The initial nested comparison failed in the legacy estimator; only its fresh,
fully validated retry enters the successful archive. The failure remains
separately recorded, with no claim that the intermittent assertion is fixed.
