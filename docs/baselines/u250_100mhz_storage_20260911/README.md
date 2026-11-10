# Response/storage optimization checkpoint

This archive contains 20 fresh comparisons using one immutable compiler,
Vitis/Vivado 2025.2, U250 `xcu250-figd2104-2L-e`, and a 10 ns constraint.
Both generators use the same source and common self-checking RTL testbench.
Resources are measured at the same normalized-wrapper Vivado OOC boundary.
All 20 comparisons are functionally matched and reward-eligible; all 20
Light-HLS implementations meet the synthesis clock constraint. This does not
claim post-route timing closure or formal equivalence for arbitrary inputs.

Compiler SHA256:
`3f53e58e7b21392af4e047adea2fd4742bb801820c71657dafbcbb8998cfd972`.

- `u250_baselines.rpt`: readable cycles, LUT, FF, and timing comparison.
- `u250_baselines.csv` and `u250_baselines.json`: measured QoR and fingerprints.
- `qor_changes.json`: comparison against `../u250_100mhz_qor_20260908/`.
  Twelve cases improve without a cycle/resource increase, seven have mixed
  tradeoffs, and one is unchanged. Seventeen reduce cycles and three retain
  the same cycle count. No case increases cycles. The strict no-regressions
  gate intentionally does not pass because resource tradeoffs remain.
- `rtl_identity_audit.json`: all 20 regenerated RTL files match their measured
  inputs byte-for-byte. No measurement fingerprints were rewritten.
- `local_regressions.json`: all ten final local suites pass.
- `app_smoke.json`: all 18 existing App functional testbenches pass separately.
- `app_capability_matrix.json`: 37 App directories, including 24 successful
  generation/syntax checks, 12 expected rejections, and one empty directory.
  Per-case functional verification is recorded separately from syntax support.

The pipeline-storage stress suite adds seven configurations with 14,336
checked outputs: repeated transactions without reset, one/two-cycle memory,
dependent dynamic bank selection, reduced buffer wraparound, dual-port
forwarding, and depth-one FIFO backpressure. See
[`Tests/PipelineStorage`](../../../Light_HLS/Tests/PipelineStorage/README.md).

Four initial parallel comparison attempts failed inside Vivado, on both
Light-HLS and Vitis RTL, with segmentation faults or an invalid enormous
allocation request. Their original logs remain in `.failed.1` directories;
all four fresh retries passed with identical compiler and input fingerprints.
The failed attempts are excluded from this archive. The root cause of the
external tool instability has not been established.

Raw projects, logs, checkpoints, immutable compiler snapshots, and the audit
outputs remain under `/tmp/lighthls-storage-qor-20260911.K7cKW9/`. Compilation
and synthesis runtimes from concurrent runs are not speed comparisons.
The Vitis combinational complete-partition case has no registered timing path;
missing WNS is not a measured Fmax. Previous archives remain unchanged.

See [the implementation notes](../../RTL_STORAGE_OPTIMIZATION_20260911.md)
for proof restrictions, resource tradeoffs, and remaining priorities.
