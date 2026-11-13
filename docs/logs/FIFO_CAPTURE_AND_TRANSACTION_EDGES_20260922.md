# FIFO capture, recurrence storage, and ordinary transaction edges

This checkpoint continues the [outer-loop work](ORDINARY_OUTER_EDGES_20260922.md).
It iterates on measured RTL bottlenecks without changing benchmark sources,
pragmas, the stream frontend shim, or the common cross-tool testbenches.

## Implementation contracts

### Request-edge capture of an internal FIFO head

An ordinary DATAFLOW consumer can capture the already-visible FIFO head on the
accepted pop edge. Previously the wrapper captured it first and the task captured
that response one cycle later. Child generation now explicitly marks eligible
internal arguments; memory analysis uses read latency zero for those arguments,
but the load still has one cycle of registered result latency. This is not global
load forwarding or a change to external `ap_memory` latency.

The wrapper directly connects the selected FIFO head to the child Q input and
removes only its redundant response register. Pop, push, empty/full checks,
instance selection, and task clock-enable gating remain unchanged. The retained
child result freezes under backpressure. Existing response-to-PHI bypasses may
sample that same accepted pop edge without changing the number of FIFO events.

Pipeline consumers, public FIFO endpoints, and cyclic task graphs retain their
registered responses. Task generation independently checks graph acyclicity:
the later performance pass has not yet populated `graph.cyclic`. Initialized
edges participate in this check too. Post-RTL validation checks provenance,
single-port/bank read-only access, clock enable, and absence of pipeline regions.
Reports expose `fifo_read_on_request` and `consumer_read_on_request`.

### Sharing a recurrence result with its PHI

A previously validated ordinary self-backedge bypass already writes the result
and PHI from the same bound operator on the transition edge. If the old PHI has
only block-local non-PHI users, the two values can share the PHI register. The
ordinary operation writes this common register unconditionally on its issue
edge, including the exit iteration. Cross-block users of the new result read the
shared register. A live-out of the old PHI prohibits sharing.

Selection also requires a real cross-block user of the new result. Otherwise
Vivado can already delete the unused result register, and changing the PHI enable
can add control logic without saving storage. This profitability guard was added
after a scalar experiment showed one extra LUT and no FF saving.

Binding, declarations, reset, expression lookup, successor PHI assignments, and
pipeline initialization agree on the alias. Reports distinguish retaining the
result **value** from retaining its original dedicated **register**.

### Fusing ordinary startup and completion states

An empty `return void` block reached only from ordinary branch predecessors can
complete on the incoming branch edge. The last memory write and completion can
share that edge; both remain under the task clock enable. Non-void returns and
pipeline predecessors keep their existing implementation.

An ordinary empty entry/preheader can initialize constant successor PHIs on the
accepted `ap_start` edge, then enter the successor directly. Only constants,
allocations, and debug instructions are allowed in the skipped entry. Every
initial PHI value must be a literal or proven hardware constant. Argument-based
initialization remains separate because argument registers are only being
latched on the start edge. Pipeline entry initialization uses its existing path.

Schedule state counts, task latency analysis, generated RTL, and post-validation
use the same plans. JSON/RPT reports include `ordinary_entry_fused`,
`ordinary_fused_void_return_count`, and `ordinary_register_alias_count`.

## Experiments not shipped

- Automatically adding a second private RAM read port reduced the private-stream
  result from 393 to 392 cycles but increased LUTs from 316 to 382, with FFs
  unchanged at 389. The port-selection experiment was removed.
- Treating constant shifts as zero-delay wiring did not change RTL for any of
  the 20 historical designs. The scalar bottleneck is cross-block live-out
  retention, not the shift delay itself. That experiment was removed too.
- Sharing a result register without a real live-out gave the scalar design
  156 rather than 155 LUTs with unchanged cycles/FFs. The final profitability
  guard excludes that case.

These rejected candidates are not silently promoted to release measurements.

## Validation scope

The unified runner now includes `TransactionEdges`. Its six fixtures test
constant and argument PHIs, zero-trip loops, early returns, signed constant
shifts, and variable shifts: 384 consecutive reset-free transactions with
input changes after acceptance, exact write counts, and completion checks.

`OrdinaryBackedge` has 18 configurations, including a new old-PHI live-out
counterexample. `FIFOEvents` has six configurations, each run both with natural
backpressure and a separate injected-stall driver: 768 calls and 12,288 output
comparisons. Injection only stalls active consumers, never the idle/start path.
The cross-tool testbench is unchanged and does not use injected stalls.

The initialized-cycle regression requires registered FIFO responses. Existing
public FIFO, conditional events, pipeline, local RAM freeze, multiple read
latency, outer-loop, and array-partition checks remain enabled. These are bounded
simulation and structural checks, not formal all-input equivalence.

All 17 unified suites and all 18 independent App smoke checks pass on the release
executable. The capability matrix remains 24 passes, 12 expected rejections, and
one non-applicable directory. The stream shim is unchanged at SHA256
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.

## Measured U250 results

Same benchmark/testbench hashes, U250 `xcu250-figd2104-2L-e`, 100 MHz,
Vitis/Vivado 2025.2, normalized external wrappers, and common Vivado OOC synthesis:

| Case | Light cycles before -> after | Vitis cycles | Light LUT before -> after | Light FF before -> after | Vitis LUT / FF |
| --- | ---: | ---: | ---: | ---: | ---: |
| Private-buffer FIFO | 409 -> 391 | 389 | 317 -> 301 | 421 -> 324 | 324 / 316 |
| Inner pipeline | 21 -> 20 | 16 | 38 -> 36 | 21 -> 20 | 20 / 20 |
| Nested DATAFLOW | 41 -> 40 | 34 | 478 -> 477 | 342 -> 341 | 237 / 184 |
| Local partitioned RAM | 43 -> 43 | 42 | 278 -> 278 | 153 -> 153 | 242 / 152 |
| Complete partition read/write | 15 -> 13 | 5 | 203 -> 137 | 42 -> 41 | 230 / 7 |
| FIFO stream | 136 -> 134 | 132 | 204 -> 202 | 139 -> 130 | 164 / 54 |
| PIPO array | 393 -> 387 | 388 | 360 -> 356 | 222 -> 213 | 275 / 126 |
| Scalar | 11 -> 11 | 4 | 155 -> 155 | 234 -> 234 | 106 / 102 |

All eight functional/interface checks pass. The automated same-contract gate
reports six improved and two unchanged cases, with no cycle/resource regression.
All constrained synthesis timing checks pass (Light WNS +8.129 to +9.332 ns);
DSP and BRAM use remain zero. These are post-synthesis, not post-route, results.
Private-buffer latency improves 4.4% and FF use 23.0% relative to the previous
checkpoint. Its remaining gap is two cycles and eight FFs versus Vitis, with
23 fewer LUTs. Other designs still have substantial gaps, as the table shows.

The [evidence archive](../baselines/u250_100mhz_capture_edges_20260922/README.md)
preserves original measurement fingerprints. Seven measurements were taken
with `Light_HLS_Top-entry`; release regeneration is byte-identical for those
seven. Scalar is freshly measured with `Light_HLS_Top-release` after the
profitability guard. Thirteen other historical designs remain byte-identical
to their earlier measured RTL. The combined identity audit covers all 21 distinct
sources without relabeling measurements as runs of another compiler binary.

## Reproduction

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-capture-regressions --keep-going
```

Raw experiments are under `/tmp/lighthls-continuous-qor-20260922.g3KrQD/`.
The release executable is `Light_HLS_Top-release`, SHA256
`d0b0d06f0be9d11446e5257f9cad9b5ea4412902faa96bbf5052e4d6b00eea25`.
It is byte-identical to the build-tree executable at this checkpoint.

The remaining scalar live-out bubbles require a separate retained-result
forwarding contract; deleting those registers would lose old iteration values.
The rejected dual-port experiment also shows why reducing child FSM cycles
alone is insufficient evidence of an end-to-end QoR improvement. No claim of
general Vitis parity or completed global optimization is made.
