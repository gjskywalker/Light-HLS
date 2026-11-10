# Light-HLS App capability matrix

`app_capabilities.json` classifies one representative source from every
directory under `Light_HLS/App`. The manifest is exhaustive by construction:
the runner rejects missing, duplicate, or stale directory entries.

Run the matrix after the functional regressions:

```bash
python3 Light_HLS/Tests/AppCapability/run_app_capability_matrix.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/light-hls-app-capabilities \
  --regression-summary /tmp/light-hls-regressions/local_regressions.json
```

Each compilable representative is processed with synthesis disabled and its
generated RTL is syntax-checked with Icarus Verilog. Unsupported examples must
fail with a declared diagnostic. A successful compilation is not automatically
training-eligible: a mapped functional regression must also pass.

The JSON output is the machine-readable contract. CSV supports analysis, while
the RPT file is the human-readable summary. Per-case build products and logs
are deliberately kept outside Git.
