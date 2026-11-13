# Ordinary outer-loop latch and response-edge optimization

This checkpoint follows the [self-backedge optimization](ORDINARY_BACKEDGE_BYPASS_20260921.md).
It removes eligible outer-loop increment-to-branch and memory-capture-to-initial-PHI
waits without changing benchmark sources, pragmas, memory latencies, or FIFO events.

## Implementation contract

### Constant-step outer feedback

`HI_OperationScheduling` now recognizes a two-input header PHI whose constant
add/subtract update is in the loop's unique conditional latch. The latch must
have exactly one successor equal to the header. The held header PHI remains a
register throughout the body and any nested loops; only the latch updates it.
All non-debug users of the update, other than that header PHI, must be non-PHI
instructions in the latch. Cross-block live-outs and nonconstant steps keep
their existing registered implementation.

Eligible updates use the existing characterized combinational selection and
70%-of-period delay budget. A later timing cut can still reinstate the register.
This lets the increment, compare, and branch share a cycle when all other
readiness requirements allow it. Memory requests still observe the old held
index; the next iteration observes the updated PHI. Neither the header nor the
latch may belong to an explicit pipeline region. An ordinary outer latch around
a separately pipelined inner loop is eligible.

Post-RTL validation reconstructs DominatorTree/LoopInfo independently and checks
the unique latch, PHI, users, and pipeline exclusions. Report entries distinguish
`ordinary_loop_latch` from the existing `ordinary_self_backedge` sampling edge.

### Response-to-PHI edge bypass

A separate edge-local plan records `(predecessor, successor PHI, load)`. The
incoming value must be a direct load in the predecessor, with a retained capture
register and a single known bank/port response. The original read request and
capture edges are unchanged. On the response capture edge, both the result
register and successor PHI can sample the same Q signal. All other users still
read the retained load register.

For a request at cycle `s` and read latency `L`, sampling is allowed only when
the final transition is exactly at `s + L`. The existing registered-load latency
remains `L + 1`. Branch operands, other successor PHIs, memory events, and
operation readiness are resolved before committing a bypass. A later transition
uses the register because Q may already contain another read response. Dynamic
bank selection, globally forwarded loads, and pipeline edge endpoints are
excluded. One- and two-cycle reads are tested.

`HI_RTLGeneration` uses the already bound memory response, inside the existing
clock-enabled FSM. Both the retained capture and new PHI assignment therefore
freeze together when a DATAFLOW task stalls. No FIFO pop, push, or response stage
is deleted. Post-RTL validation checks the exact capture edge, retained register,
unique single-bank binding, PHI incoming edge, and pipeline exclusions.

JSON/RPT exposes `ordinary_read_phi_bypass_count`, the load/PHI pair, predecessor,
successor, sample cycle, and retained-register status. As before, the parent
DATAFLOW report summarizes child performance but does not aggregate child bypass
tables.

## Validation

- All 16 unified regression suites pass, including `FIFOEvents` depth-one
  backpressure, exact event counts, private-memory freeze, and repeated calls.
- The new [OrdinaryLoopEdges suite](../../Light_HLS/Tests/OrdinaryLoopEdges/README.md)
  passes 20 configurations: 1,280 reset-free calls and 16,492 output writes.
  The previous release passes the same oracle. All 20 cycle comparisons are
  non-regressing; the fixed nested loop improves from 210 to 178 cycles at read
  latency one, and from 226 to 194 at latency two.
- Directed checks cover zero/dynamic outer trips, descending accesses, dynamic
  steps, load live-outs, two successor edges, later division, inner pipelines,
  two different responses sharing a port, and a load-dependent branch that must
  retain the extra capture wait.
- All 18 independent App smoke checks pass. The 37-directory capability matrix
  retains 24 passes, 12 expected rejections, and one non-applicable directory.
- Of 20 historical measured sources, 13 match the September 14 RTL, four match
  the newer feedback checkpoint, and scalar matches the newer backedge release.
  The two remaining historical sources and the additional private-buffer source
  are freshly measured. All three regenerated measured RTL files are byte-identical.

These are bounded simulation/structural checks, not formal all-input equivalence.

An exploratory `input[i]`/`input[i+8]` two-response fixture hit the pre-existing
`handleSAREAccess` assertion in legacy address analysis. The previous executable
reproduces the same assertion before backend scheduling. The committed response
ordering test uses constant addresses 0 and 1 to isolate backend semantics. The
address-analysis limitation is not fixed or counted as a passing test here.

## Fresh U250 measurements

Same original sources and testbenches, U250 `xcu250-figd2104-2L-e`, 100 MHz,
Vitis/Vivado 2025.2, and normalized-wrapper common Vivado OOC synthesis:

| Case | Light cycles before -> after | Vitis cycles | Light LUT before -> after | Light FF before -> after | Vitis LUT / FF |
| --- | ---: | ---: | ---: | ---: | ---: |
| Private-buffer FIFO | 504 -> 409 | 389 | 329 -> 317 | 508 -> 421 | 324 / 316 |
| Inner pipeline | 21 -> 21 | 16 | 38 -> 38 | 21 -> 21 | 20 / 20 |
| Nested DATAFLOW | 41 -> 41 | 34 | 478 -> 478 | 342 -> 342 | 237 / 184 |

All functional comparisons, interface checks, and non-regression gates pass.
DSP and BRAM use stay at zero. Light-HLS synthesis WNS is +8.717, +8.886,
and +8.826 ns respectively. These are post-synthesis results, not post-route
timing closure. The other two cases change emitted RTL but not measured QoR.

Private-buffer end-to-end latency improves by 18.8%, LUT by 3.6%, and FF by
17.1%. Its cycle gap to Vitis decreases from 29.6% to 5.1%; FF still exceeds
Vitis by 33.2%. This is a local improvement, not evidence of general parity.

## Remaining work

Generated un-stalled child FSM counts are now 226 / 82 / 178 cycles for
`load_slow` / `private_buffer` / `store_slow`, versus 290 / 82 / 210 before.
The previous Vitis child HLS reports list 225 / 69 / 161. These child counts
are diagnostic, not measured end-to-end latency.

The producer outer iteration falls from nine to seven cycles; the consumer
falls from thirteen to eleven. The producer is now close to the Vitis child
schedule. The consumer still has a request/response boundary before its eight
compute iterations and output write. A next optimization should explicitly
model the FIFO handshake-to-first-use path, preserving stalls and retained
live-outs, then address private-buffer drain scheduling and residual FF storage.
Do not remove another state merely because an unstalled waveform appears safe.
General multi-block arithmetic bypass, nonconstant feedback, SDC scheduling,
and RL search remain outside this change.

## Reproduction and evidence

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-outer-regressions --keep-going
python3 Light_HLS/Tests/OrdinaryLoopEdges/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-outer-edges
```

Use the private-buffer FairComparison invocation in the
[FIFO checkpoint](FIFO_EVENT_PRESERVATION_20260921.md). The pipeline batch accepts
`--case inner`; nested DATAFLOW uses
`Tests/DataflowBackend/nested_pipeline_partition_dataflow.cc` and the unchanged
`App/light_hls_dataflow/vector_pipeline_partition_dataflow_tb.v`, with
`DATAFLOW_VITIS_DUAL_PORT` and `DATAFLOW_VITIS_HOLD_START_UNTIL_DONE` for Vitis.

The [compact evidence archive](../baselines/u250_100mhz_outer_edges_20260922/README.md)
contains unchanged generated reports, fingerprints, and identity audits.
Raw root: `/tmp/lighthls-outer-edges-20260922.nuyJGJ/`; executable:
`Light_HLS_Top-v1` (also identical to the build-tree binary), SHA256:
`02193fe2d7eae6a840e61d457006d2e447f0c97b3d4d2d973be9e9557a106958`.
The stream header remains unchanged at SHA256
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.
Release measurements are `u250-v1/private`, `u250-v1/pipeline/inner`, and
`u250-final/dataflow_nested`. The unsuccessful `u250-v1/dataflow_nested` invocation
used an incorrect testbench path and is not a release measurement.
