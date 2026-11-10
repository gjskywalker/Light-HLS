# U250 correctness checkpoint, September 29

This archive records bounded correctness closure, not RL results or board
timing signoff. See [the engineering notes](../../RTL_CORRECTNESS_CLOSURE_20260929.md)
for interface semantics, limitations, and reproduction commands.

- `manifest.json`: release compiler fingerprint, checked artifact hashes,
  routed timing/resources, and routed functional-output counts.
- `local_regressions.json` / `.rpt`: complete 27/27 suite run.
- `app_smoke.json`: 18/18 supported App cases.
- `transaction_edges.json`: 1152 ordinary pulse/held/gapped calls with reset
  interruption and recovery.
- `pressure.json`: four pressure kernels at 100 and 250 MHz, including
  continuous-start and reset-recovery streams for DSP map and DATAFLOW.
- `negative_control.log` / `negative_control_rtl.v`: unchanged pre-fix RTL
  fails on the first output of the second scalar-argument transaction.
- `sustained-*.json`: independent ready, done, and final-commit events for
  Light-HLS and Vitis, with all 8192 C-reference outputs checked per stream.
- `map-100`, `bank-250`, `dataflow-250`: unchanged kernel sources, shared
  registered harness, physical reports, generated Tcl/XDC, and verification
  fingerprints for both generators.

All six registered implementations pass setup, hold, timing coverage and
routing checks. All six routed functional netlists pass 8192 C-reference
outputs using UNISIM; static timing, not SDF simulation, checks path delays.
DSP pipelining and intentionally retained unloaded observer nets still produce
advisory DRC warnings; these are preserved in the reports.

The flow is `vivado_registered_pressure_context_v1`: on-chip sources, one-cycle
RAM response, retained observers, and a real BUFG. Kernel and shell resource
counts are separate. These results must not be mixed with standalone
`vivado_ooc_normalized_v3_pre_synth_io20` QoR. The earlier virtual-input hold
failures and their ineligibility are unchanged.

Raw DCPs, full simulator traces, failed preliminary runs, and frozen compilers
remain in `/tmp/lighthls-correctness-20260929.kZl9Xq`. The archive gate reparses
routing/timing reports, checks source/RTL/netlist/DCP fingerprints, and rejects
incomplete matrices, severe DRC findings and failed physical results:

```bash
python3 Light_HLS/Tests/FairComparison/archive_correctness_checkpoint.py \
  --run-root /tmp/lighthls-correctness-20260929.kZl9Xq \
  --regression-dir /tmp/lighthls-correctness-20260929.kZl9Xq/release-verified \
  --output /tmp/fresh-correctness-archive
```
