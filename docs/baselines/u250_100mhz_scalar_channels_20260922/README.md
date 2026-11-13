# U250 scalar/channel optimization evidence

See the [implementation and measured-results report](../../logs/RTL_SCALAR_AND_CHANNEL_OPTIMIZATION_20260922.md).

- `before.*` and `after.*`: all 21 cases, preserving original comparison IDs,
  source/testbench/compiler fingerprints, and measurement validity.
- `qor_delta.json`: nine improvements, two explicit tradeoffs, ten unchanged
  cases; all measured timing constraints met and no cycle regression.
- `release_rtl_coverage.json`: all 21 candidate RTL files byte-identical to
  regeneration with the release compiler. Reused measurements retain their
  actual compiler fingerprints and are not reissued as new measurements.
- `raw_manifest.json`: original report paths; the two repaired testbenches use
  freshly remeasured old-compiler baselines as well as fresh candidates.
- `local_regressions.*`, `app_smoke.json`, and `app_capability_matrix.json`:
  release correctness and supported-scope evidence.
- `scalar_bank_events.json`, `scalar_returns.json`, `pipo_banks.json`, and
  `fifo_events.json`: directed functional/structural stress summaries.
- `scalar_schedule.rpt`: the current human-readable schedule; measured cycles
  are in the comparison archive rather than inferred from a dynamic loop bound.

The archived files are unmodified copies of tool outputs. Raw Vivado/Vitis logs,
generated RTL, waveforms, and intermediate files remain outside Git under
`/tmp/lighthls-vitis-gap-20260922.3vMrGJ/` and the historical paths in the manifest.
The archive is not a substitute for retaining those raw directories long term.

```bash
python3 Light_HLS/Tests/FairComparison/build_measured_checkpoint.py \
  --manifest docs/baselines/u250_100mhz_scalar_channels_20260922/raw_manifest.json \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-scalar-channel-checkpoint
```

This command requires the referenced raw measurements. It refuses to overwrite
existing outputs, rejects stale sources and unsupported explicit configurations,
and fails on candidate RTL mismatch. The QoR audit exposes resource tradeoffs
instead of silently asserting that every individual metric improves.
