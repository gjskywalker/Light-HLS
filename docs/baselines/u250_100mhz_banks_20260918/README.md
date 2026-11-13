# Ordinary-bank optimization checkpoint

This archive contains **one freshly measured** U250 comparison for the unchanged
`backend_array_partition_local` App. It uses Vitis/Vivado 2025.2, part
`xcu250-figd2104-2L-e`, 10 ns, a common self-checking RTL testbench, and the common
normalized-wrapper Vivado OOC synthesis flow. Both generated RTL designs pass
functional and normalized-interface checks; the comparison is reward-eligible.
Synthesis timing passes, but this is not post-route timing closure.

Final compiler SHA256:
`fbe1e73f68f671f040aa636ac496511d4ce567ebeb231ef3550be870307de8cd`.

| Metric | Previous Light-HLS | Current Light-HLS | Vitis |
| --- | ---: | ---: | ---: |
| Observed cycles | 59 | 43 | 42 |
| LUT | 596 | 428 | 242 |
| FF | 310 | 184 | 152 |

- `u250_baselines.json`, `.csv`, `.rpt`: the fresh single-case measurement.
- `previous_local_baseline.json`: the previous raw local comparison normalized
  separately without changing its compiler fingerprint or the original archive.
- `qor_changes.json`: the strict no-metric-regressions comparison passes.
- `rtl_identity_audit.json`: regenerated local RTL exactly matches the fresh
  measured RTL.
- `rtl_impact_audit.json`: cross-version audit of all 20 previous cases. Nineteen
  are byte-identical; `array_partition/local` is the sole difference. The audit's
  aggregate `valid: false` means that not all old RTL files match, as expected;
  it is not a failed functional test. Those 19 designs were not resynthesized.
  No historical measurement fingerprints were rewritten.
- `local_regressions.json`: all 11 final suites pass.
- `ordinary_bank_regressions.json`: all 20 new configurations pass, totaling
  1,280 calls and 9,664 written outputs with additional memory/event checks.
- `app_smoke.json`: all 18 independent App RTL testbenches pass.
- `app_capability_matrix.json`: generation/syntax successes and expected
  rejections remain separately classified, not promoted to universal functional
  verification.

Raw logs and immutable compiler snapshots are under
`/tmp/lighthls-bank-qor-20260918.ZR3t1F/`. The earlier intermittent estimator and
linker failures remain unresolved; this change adds crash stack traces but does
not claim to fix either. Development runs are kept separately from final
validation. See [the implementation notes](../../logs/ORDINARY_BANK_OPTIMIZATION_20260918.md)
for proof boundaries, reproduction, and next steps.
