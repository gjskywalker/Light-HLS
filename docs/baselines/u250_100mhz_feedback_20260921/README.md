# September 21 ordinary-feedback optimization evidence

See the [checkpoint notes](../../ORDINARY_FEEDBACK_OPTIMIZATION_20260921.md)
for implementation boundaries, interpretation, reproduction, and remaining work.

All twelve generated files below are byte-identical copies of raw outputs:

- `before_u250_baselines.json`: six prior measurements under the same source,
  testbench, part, clock, and synthesis contract. Local-array comes from the
  September 18 payload checkpoint; private-buffer FIFO comes from the earlier
  September 21 FIFO checkpoint; the other four come from September 14.
- `u250_baselines.{json,csv,rpt}`: six fresh Vitis/Vivado 2025.2 comparisons,
  U250 `xcu250-figd2104-2L-e`, 100 MHz, normalized-wrapper Vivado OOC synthesis.
- `qor_delta.json`: six improvements; no cycle/LUT/FF/DSP/BRAM regressions;
  all six synthesis timing constraints met.
- `local_regressions.{json,rpt}`: all 14 local regression suites pass.
- `ordinary_feedback.json`: 17 configurations and 1,088 calls, source hashes,
  per-case read latency, clock constraint, and selected feedback-wire counts.
- `app_smoke.json`: all 18 independent App RTL checks pass.
- `historical_rtl_identity.json`: 15 of 20 historical files are byte-identical.
  The overall `valid: false` is intentional because the five optimized sources
  differ and are freshly measured instead of inheriting old QoR numbers.
- `final_rtl_identity.json`: all six freshly measured RTL files are reproduced
  byte-for-byte by the frozen final compiler, including private-buffer FIFO.
- `local_cells.tsv`: primitive inventory from the final normalized local-array
  checkpoint. It contains 153 FFs: 128 RAM outputs, 20 held induction bits,
  and five FSM/status bits. Use the utilization report, not the count of RAM
  primitive names, for physical LUT-as-memory utilization.

Compiler SHA256:
`068c78311810d08048ca0c49f867186cf628e426bcecc7852a72f75b6444daff`.
Unchanged runtime stream header SHA256:
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.

Raw root: `/tmp/lighthls-feedback-qor-20260921.5am2BK/`.
Final measurements: `u250-final/`; regressions: `regressions-release/`;
before/final archives: `before-archive/` and `final-archive/`.
The initial `u250-local-v1/` experiment is not used as the final measurement.
Earlier failed fixture attempts remain separate, including unsupported-boundary
tests and a pre-change compiler reproduction of invalid PHI/return LLVM IR.
Large tool projects, netlists, and compiler binaries are not committed.

Functional checks are bounded simulations, not formal equivalence. Synthesis
timing is not post-route timing. `reward_eligible` denotes harness acceptance,
not Vitis superiority or an implemented RL search.
