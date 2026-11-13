# Local-memory payload optimization checkpoint

One fresh comparison uses the unchanged local-array App, common RTL testbench,
Vitis/Vivado 2025.2, U250 `xcu250-figd2104-2L-e`, and 10 ns. Resource results
use the same normalized-wrapper Vivado OOC boundary. Functional/interface
checks and synthesis timing pass; the result is reward-eligible.

| Metric | Previous Light-HLS | Current Light-HLS | Vitis |
| --- | ---: | ---: | ---: |
| Observed cycles | 43 | 43 | 42 |
| LUT | 428 | 297 | 242 |
| FF | 184 | 181 | 152 |

Final compiler SHA256:
`24266e01ef47fcde15715edbc5777b9841dc38f92b79be45c1e59b4a3810091d`.

- `u250_baselines.json`, `.csv`, `.rpt`: fresh normalized measurement.
- `previous_local_baseline.json`: previous measurement with its original compiler
  fingerprint, normalized separately for the same-case comparison.
- `qor_changes.json`: strict no-cycle/resource-regressions check passes.
- `rtl_identity_audit.json`: fresh measured local RTL is exactly reproducible.
- `rtl_impact_audit.json`: nineteen of twenty historical measured RTL files are
  byte-identical; local is the sole expected mismatch. The audit's aggregate
  false value is an identity difference, not failed functional simulation.
  Those nineteen cases were not resynthesized.
- `local_regressions.json`: twelve final suites pass.
- `local_write_regressions.json`: eight fixtures, 512 calls, 8,192 output checks.
- `app_smoke.json`: eighteen App self-checking RTL testbenches pass.
- `register_attribution.json`: FF groups from the actual normalized synthesized
  checkpoints. Primitive counts must not be mistaken for packed FPGA LUT cost.
- `known_stream_failure.rpt`: separately preserved FIFO event-loss evidence and
  an unresolved ScalarEvolution assertion. This source is not a positive test.

Raw projects, logs, RTL, checkpoints, and compiler snapshots remain under
`/tmp/lighthls-local-mux-qor-20260918.4T7n35/`. Earlier archives are untouched.
See [implementation and limitations](../../logs/LOCAL_MEMORY_PAYLOAD_OPTIMIZATION_20260918.md).
