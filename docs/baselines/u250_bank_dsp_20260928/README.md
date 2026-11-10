# U250 bank/DSP follow-up evidence

See [the implementation notes](../../RTL_BANK_DSP_EVALUATION_20260928.md) for
algorithms, measured results, reproduction paths and known limitations.

- `before/`: two frozen-old-compiler comparisons rerun under the new
  pre-synthesis clock/IO contract.
- `after/`: four new measurements at 100, 200 and 250 MHz. Three include
  placement and routing. This is not the old unconstrained-synthesis flow.
  Legacy CSV/RPT simulation-cycle columns describe the done edge; explicit
  final-commit events remain in JSON and the implementation notes' table.
- `implementation_audit.json`: routed metrics, timing coverage, artifact
  fingerprints and internal timing diagnostics. Full IO hold failures are
  retained; `all_ooc_constraints_met` is false.
- `release_rtl_coverage.json`: all four measured RTL files are byte-identical
  to the final compiler output. Original measurement fingerprints and failed
  reward eligibility are not rewritten.
- `local_regressions.json` / `.rpt`: 27/27 suites pass, including bounded bank
  conflict proof checks, latency-two memory and arithmetic edge cases.
- `app_smoke.json`: 18 supported App cases pass.
- `dataflow_sustained_250.json`: 32 transactions and 8192 C-reference outputs
  on both generators; final-commit intervals 261/260 cycles (Light/Vitis).
- `dataflow_sustained_before_100.json`: identical protocol applied to the old
  100 MHz DATAFLOW RTL; intervals 260/260. This is not a same-clock physical
  before/after comparison with the 250 MHz result.
- `known_scalar_held_start_failure.json`: an additional non-DATAFLOW probe
  fails on the second scalar-argument capture. It is not a passing test.
- `native_vivado_metrics.txt` / `native_constraints.xdc`: direct
  `--synthesis enable` smoke evidence, separate from normalized-wrapper QoR.
- `constraint_summary.json`: measured frequency coverage, not the skill's
  plan-only grid and not a best-Fmax claim.

Raw reports, generated RTL and checkpoints remain at
`/tmp/lighthls-next3-20260928.t1orVg`. Failed initial constraint bring-up runs
in that directory are excluded. Large compiler binaries and DCPs are not
duplicated into this compact archive.
