# September 21 same-edge backedge evidence

See the [checkpoint notes](../../logs/ORDINARY_BACKEDGE_BYPASS_20260921.md) for the
safety contract, measured improvement, remaining gaps, and reproduction.

The seventeen generated files in this directory are byte-identical copies:

- `before_u250_baselines.json`: scalar/private-buffer measurements from the
  immediately preceding ordinary-feedback checkpoint.
- `u250_baselines.{json,csv,rpt}`: fresh release comparisons using unchanged
  source/testbench inputs, Vitis/Vivado 2025.2, U250 `xcu250-figd2104-2L-e`,
  100 MHz, and the common normalized-wrapper Vivado OOC synthesis flow.
- `qor_delta.json`: both cases improve in cycles, LUT, and FF; no resource/cycle
  regressions; both synthesis constraints met.
- `local_regressions.{json,rpt}`: all 15 unified suites pass, including the
  37-directory capability matrix with its original expected-rejection contract.
- `ordinary_backedge.json`: 16 final-compiler configurations, 1,024 calls,
  16,384 outputs, source hashes, bypass counts, and optional prior-compiler cycle
  comparisons. Fourteen configurations additionally run 896 baseline calls.
  The two narrow cases explicitly record the prior invalid-IR limitation.
- `app_smoke.json`: all 18 independent App RTL checks pass.
- `historical_rtl_identity.json`: 15 of 20 September 14 designs match exactly.
- `previous_feedback_rtl_identity.json`: four newer local-array/partition/FIFO/
  PIPO designs match exactly; scalar and private-buffer change. Together the
  two historical audits establish 19 unchanged sources in the existing 20-case
  set. Their overall `valid: false` records intentional RTL changes, not an
  inherited functional failure or permission to reuse old QoR for changed RTL.
- `release_rtl_identity.json`: both newly measured RTL files match the release
  compiler byte-for-byte. Old fingerprints are not overwritten.
- `private_dataflow_graph.rpt`: generated child-FSM latency diagnostics.
- `vitis_{load_slow,store_slow,private_buffer}_csynth.xml`: corresponding Vitis
  child reports. These estimates are diagnostic, not substitutes for the common
  end-to-end simulation cycles or Vivado resource measurements.
- `narrow_before_verify.log`: the previous compiler's bitcode verification
  failure on the narrow cross-block fixture, reproduced before the repair.

Release compiler SHA256:
`8db9a9f74bb67e570154bc242a0ec9d5e0cdbe8023b64fe5bac9728c2ae6e5c8`.
Unchanged stream-header SHA256:
`451d510379d8e869a611be66d23c617e82588adc3dadab374021f3b2320f7090`.

Raw root: `/tmp/lighthls-backedge-qor-20260921.CGzauP/`.
Release measurements are `u250-release/`, regression results are
`regressions-release/`, and the frozen executable is `Light_HLS_Top-release`.
Earlier candidates, failed regressions, and broad width-repair attempts remain
separate and are not relabeled as final results. Large compiler binaries,
netlists, and tool projects remain outside Git.

These checks do not prove formal equivalence or post-route timing. The
`reward_eligible` field denotes successful harness acceptance, not Vitis parity
or an implemented RL search.
