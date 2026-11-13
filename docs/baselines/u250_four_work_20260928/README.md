# Four-workstream release evidence

See [the checkpoint report](../../logs/RTL_FOUR_WORK_CHECKPOINT_20260928.md) for
implementation, safety contracts, results, limitations, and reproduction.

| Artifact | Meaning |
| --- | --- |
| `before.json` | Preserved preceding 21-case checkpoint |
| `app/u250_baselines.{json,csv,rpt}` | All 21 current App/common-flow results |
| `qor_delta.json` | Same-contract audit: 7 improved, 3 tradeoffs, 11 unchanged |
| `pressure/u250_baselines.{json,csv,rpt}` | Ten DSP/BRAM clock/design comparisons |
| `release_rtl_coverage.json` | Final compiler versus all 31 measured RTL files |
| `synthesis_reuse.json` | Original artifacts and hashes for adapter-only revalidation |
| `binding_synthesis.json`, `binding_release_identity.json` | Physical before/after binding test and release identity |
| `local_regressions.{json,rpt}`, `app_smoke.json` | 24 regression suites and 18 independent smoke tests |
| `app_capability_matrix.json` | Supported and deliberately rejected App cases |
| `binding_cost.json`, `dataflow_completion.json` | New correctness/cost regressions |
| `pressure_local.json`, `queued_transactions.json` | Sixteen local configurations and queued event traces |
| `constraint_plan.json` | Plan only; not measured results |
| `constraint_summary.json` | Measured DSP-map sweep, objective, and selected point |
| `release-manifest.json` | Original raw report paths used for identity checking |

Targets use U250, Vitis/Vivado 2025.2, and the common normalized-wrapper OOC
flow. Estimator resources are not substituted for physical synthesis resources.
Pressure events separate first observed done from final memory commit. The
10-case pressure archive and 21-case App archive have different benchmark sets;
do not average them into a before/after speedup.

Some physical results were generated with an earlier frozen candidate. They
retain that original executable hash and are covered by the final compiler's
byte-identical RTL proof. Adapter revalidation is explicitly not a fresh
synthesis run. Raw projects stay outside the repository, and their `/tmp`
paths may not survive cleanup; compact provenance is retained here.
