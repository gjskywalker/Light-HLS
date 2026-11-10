# Scalar, bank-response, and DATAFLOW RTL optimization

This checkpoint follows the [capture-edge checkpoint](FIFO_CAPTURE_AND_TRANSACTION_EDGES_20260922.md).
Vitis RTL and synthesis-report profiling identified unnecessary memory-response
capture, transaction-boundary states, and internal payload gating. The changes
are backend transformations; benchmark C/C++ sources, pragmas, target part,
clock period, and the stream frontend shim are unchanged.

## Implementation and safety contracts

### Scalarized external array banks

Fully scalarized external banks (bank count equals depth and bank depth is one)
already expose scalar input wires. Without an explicit read-latency override,
the backend now models those inputs with zero read latency. They can fan out
without consuming a RAM request slot. A read may sample the old value on the
same edge that commits a following write; RAW and WAW ordering still require a
later edge. Ordinary RAM, local RAM, and explicit latency overrides retain their
existing behavior. Reports expose `combinational_scalar_reads`.

Forwarding requires all uses to sample on the response edge. Otherwise the load
keeps its capture register. Dynamic bank selection uses the correct address,
not bank zero. Pipeline retirement can be stage zero when every last effect is
already committed there. No completion is moved before an outstanding effect.

### Ordinary loop live-outs and transaction edges

A pure expression may remain combinational across the final edge of a
single-block ordinary loop when its roots are held PHIs, captured arguments,
or constants, and its escaping users belong to the terminal exit. Those old
PHIs are excluded from recurrence-result register sharing. Expression selection
is deterministic and rechecks characterized paths against the 70% clock-period
budget. Memory-dependent and unsupported paths keep their registers.

A pure, timed ordinary preheader can initialize successor PHIs on accepted
start. Separate typed expressions use the live input ports on that edge;
expressions during execution continue to use captured arguments. A timed pure
return block can complete on its incoming branch edge. Scheduling recursively
checks return operand readiness, and post-validation checks both transformations.
The registered return value and its existing hold behavior are preserved.

Reports distinguish all fused returns from the backward-compatible
`ordinary_fused_void_return_count`, and record held live-out PHIs and entry fusion.

### Dynamic synchronous-bank responses

Pipeline lifetime analysis already retains a dynamic read address through its
response edge. Eligible latency-one loads can therefore forward the selected
bank's Q directly into a same-edge consumer. The sampling-edge address selects
the bank; later uses still require capture. Latency-two and ordinary dynamic-bank
fallbacks remain conservative. The dependent-address stress test checks both
latencies over 64 transactions and 2,048 results per configuration.

### FIFO head sampling and PIPO storage

Eligible ordinary consumers in an acyclic internal FIFO graph can forward a
stable FWFT head to an operation on the accepted pop edge. The wrapper jointly
gates pop, push, memory effects, and task progress with the task clock enable.
All uses must have that single sampling edge. Other loads retain capture.
Pipeline, public, and initialized cyclic FIFO endpoints keep their previous
response contract. Post-validation checks provenance and the accepting edge.

PIPO transaction buffers now share one payload array, with ownership selecting
an address offset, and one synchronous response register per partition bank.
The response retains the bank selected on the request edge. Payload storage has
no reset. Depth-one scalar peek remains supported; non-power-of-two depth is
handled by an offset, not an assumed high address bit.

An internal PIPO write payload with one static producer per bank/port need not
be gated by state and bank selection. CE/WE and task acceptance still qualify
every actual write. Child configuration explicitly identifies internal PIPO
arguments; public outputs, multiple writers, and scalar PIPO banks retain their
existing gating. Binding/validation agree on the contract, and reports separate
`direct_internal_pipo_write_port_count` from local RAM write ports.

## Fair-comparison testbench correction

The complete-pipeline and multidimensional array testbenches previously held
`ap_start` across an extra acceptance edge after completion. Faster Light-HLS
RTL exposed an unintended second transaction. Releasing start immediately at
the first observed done is also insufficient for Vitis variants that announce
done before their final memory-write edge.

Both corrected benches now allow that final commit only when needed, then
release start before another acceptance edge. Exact write counts and all output
checks remain. They retain the original first-observed-done cycle metric.
The old release executable and Vitis were freshly rerun with these same repaired
benches; the archive does not compare mismatched testbench fingerprints.
Discarded runs from the first, insufficient repair are not release evidence.

## Measured results

U250 `xcu250-figd2104-2L-e`, 100 MHz, Vitis/Vivado 2025.2, identical benchmark
sources, normalized external wrappers, and the common Vivado OOC synthesis flow:

| Case | Light cycles before -> after | Vitis cycles | Light LUT before -> after | Light FF before -> after | Vitis LUT / FF |
| --- | ---: | ---: | ---: | ---: | ---: |
| Scalar | 11 -> 4 | 4 | 155 -> 137 | 234 -> 135 | 106 / 102 |
| Complete partition read/write | 13 -> 5 | 5 | 137 -> 164 | 41 -> 10 | 230 / 7 |
| Complete partition | 3 -> 2 | 0 | 110 -> 109 | 5 -> 3 | 92 / 0 |
| Complete partition pipeline | 7 -> 5 | 3 | 92 -> 89 | 47 -> 9 | 131 / 3 |
| Multidimensional complete partition | 5 -> 3 | 1 | 182 -> 166 | 139 -> 7 | 147 / 2 |
| Local partitioned RAM | 43 -> 41 | 42 | 278 -> 275 | 153 -> 155 | 242 / 152 |
| FIFO stream | 134 -> 132 | 132 | 202 -> 198 | 130 -> 64 | 164 / 54 |
| PIPO array | 387 -> 387 | 388 | 356 -> 337 | 213 -> 149 | 275 / 126 |
| Pipeline/partition DATAFLOW | 38 -> 37 | 34 | 481 -> 233 | 339 -> 174 | 223 / 175 |
| Nested DATAFLOW | 40 -> 39 | 34 | 477 -> 238 | 341 -> 176 | 237 / 184 |
| Private-buffer FIFO | 391 -> 391 | 389 | 301 -> 300 | 324 -> 324 | 324 / 316 |

The complete [21-case archive](baselines/u250_100mhz_scalar_channels_20260922/after.csv)
also includes the ten unchanged designs. Nine cases improve without any
cycle/resource increase; two are explicit tradeoffs; ten are unchanged.
No case has a cycle regression. Complete read/write uses 27 more LUTs to remove
eight cycles and 31 FFs. Local RAM uses two more FFs while saving two cycles and
three LUTs. These are not described as a no-metric-regression checkpoint.

Every constrained synthesis run meets timing; Light-HLS WNS is +8.118 to
+9.607 ns. DSP and BRAM counts remain zero. These are post-synthesis results,
not routed timing or resource guarantees. Cycle counts are testbench-observed
transaction completion, not final-memory-commit latency or universal latency
bounds for other input distributions.

## Validation and provenance

- All 20 unified regression suites pass on the release executable.
- All 18 independent App RTL smoke tests pass.
- The App capability matrix remains 24 passes, 12 expected rejections, and one
  non-applicable directory; unsupported cases are not silently counted as passes.
- `ScalarBankEvents`: 576 transactions across ordinary/pipelined read-write and
  dynamic read-fanout cases with default scalar latency and explicit latencies 1/2.
- `ScalarReturns`: 384 transactions covering live-out old/new values, zero-trip
  and dynamic loops, argument changes after acceptance, output hold, and both
  100 MHz and the tightest characterized 3 ns library period.
- `PIPOBanks`: 8,192 request/hold cycles at depths 1, 3, 16, and 64, including
  concurrent reads/writes, bank changes, response retention, and scalar peek.
- Existing `FIFOEvents`: 384 natural-backpressure calls plus 384 injected-stall
  calls, checking 12,288 outputs and exact token/memory events.
- All 21 archived candidate RTL files regenerate byte-identically with the
  release executable. Ten unchanged measurements are reused only with this
  identity proof; original measurement compiler fingerprints remain intact.

The release binary SHA256 is
`6e4900d66e2009fafd99558653b72159eda880d571cc061451780d5cd9f8bfc3`.
The stream shim remains
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.
Tests are bounded simulation and structural checks, not formal all-input
equivalence. The 3 ns functional tests are not a separate 3 ns synthesis claim.

## Remaining limits

This is not universal Vitis parity. Small pipeline designs still expose
controller/start/drain overhead (for example, 18 versus 16 cycles and 34 versus
9 LUTs for `pipeline/ii1`). The recurrence case remains 19 versus 16 cycles and
87 versus 39 FFs. Return-hold storage contributes to that FF gap and to the scalar
case; removing it would change the current Light-HLS interface behavior.

The PIPO array is one cycle faster than Vitis but still uses about 23% more LUTs.
Nested DATAFLOW has essentially equal LUT/FF cost but still takes five more
cycles. These residuals are recorded, not hidden by averaging or by changing
benchmarks. Future work can target controller/counter sharing and transaction
launch/drain behavior under an explicit interface contract.

## Reproduction

```bash
cmake --build Light_HLS/Tests/Light_HLS_Top/build -j2
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-scalar-channel-regressions --keep-going
python3 Light_HLS/Tests/PipelineStorage/run_app_smoke.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-scalar-channel-smoke
```

Raw measurements and preserved candidate binaries are under
`/tmp/lighthls-vitis-gap-20260922.3vMrGJ/`. The evidence archive retains a raw
measurement manifest. With those raw directories available,
`FairComparison/build_measured_checkpoint.py` regenerates all candidate RTL,
archives both sides, and runs the same-contract QoR audit without relabeling
old measurements as executions of a new compiler.
