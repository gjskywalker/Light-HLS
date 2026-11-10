# September 21 FIFO correctness and U250 evidence

This archive records one fresh comparison: the previously failing private-buffer
FIFO source, top `local_dataflow`. See
[the checkpoint notes](../../FIFO_EVENT_PRESERVATION_20260921.md) for the safety
contract, reproduction commands, remaining gaps, and comparison conditions.

- `u250_baselines.{json,csv,rpt}`: fresh common normalized-wrapper U250
  synthesis, Vitis/Vivado 2025.2, 100 MHz. Light-HLS: 760 cycles, 354 LUT,
  522 FF. Vitis: 389 cycles, 324 LUT, 316 FF. Both use zero DSP/BRAM tiles and
  have positive synthesis WNS. All 64 transactions and 1,024 outputs agree.
- `local_regressions.{json,rpt}`: all 13 suites pass with the final compiler.
- `fifo_events.json`: five positive cases, 320 calls, 5,120 outputs; two
  negative cases; six IR checkpoint event counts and source/header hashes.
- `app_smoke.json`: all 18 independent App RTL checks pass.
- `september14_rtl_identity.json`: 19 of 20 historical RTL files are identical.
  Its overall `valid: false` is intentional: the local-array file was already
  improved in September 18's checkpoint, not changed by this FIFO update.
- `september18_local_rtl_identity.json`: the local-array file matches that
  latest measured baseline exactly. Together the two audits cover the existing
  20-case set without claiming fresh synthesis of those designs.
- `private_rtl_identity.json`: the final compiler reproduces the newly measured
  private-buffer RTL byte-for-byte.

All ten generated files above are byte-identical copies of the original run
outputs. Old measurement fingerprints are not overwritten. JSON files retain
raw artifact paths for inspection on the current machine. The raw root is
`/tmp/lighthls-fifo-events-20260921.sXuNPi/`; final comparison artifacts are
under `u250-final/private/`. Earlier failed attempts are retained separately,
not relabeled as successful runs. Large tool projects/netlists stay outside Git.

Compiler SHA256:
`8c03681db4a57cfa4ed67ddd3df5cfaea04564a4b32c115897918fa915053eeb`.
Runtime frontend header SHA256:
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.

The `reward_eligible` field means functional and normalized-interface checks
passed under the existing harness. It does not mean Light-HLS outperforms Vitis
or that this checkpoint implements RL. These measurements expose remaining
cycle/register costs after restoring correct FIFO semantics.
