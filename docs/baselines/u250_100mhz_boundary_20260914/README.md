# Pipeline boundary optimization checkpoint

This archive contains 20 completed comparisons using one immutable compiler,
Vitis/Vivado 2025.2, U250 `xcu250-figd2104-2L-e`, and a 10 ns constraint.
Both generators use the same source and common self-checking RTL testbench;
resources use the same normalized-wrapper Vivado OOC synthesis boundary.
All 20 comparisons pass the functional and normalized-interface checks and
are reward-eligible. All 20 Light-HLS designs meet the synthesis constraint.
This is not post-route timing closure or formal equivalence for all inputs.

Compiler SHA256:
`9b68280ee523075a1a13074c6e71782070fcee2b3188e8eaddbbba96a81fc98d`.

- `u250_baselines.rpt`: readable cycles, LUT, FF, and timing comparison.
- `u250_baselines.csv` and `u250_baselines.json`: measured QoR and fingerprints.
- `qor_changes.json`: same-contract comparison against the September 11 archive.
  Thirteen cases improve without any cycle/resource increase, five have mixed
  tradeoffs, and two are unchanged. Eighteen reduce cycles; none increase
  cycles. The strict no-metric-regressions gate does not pass.
- `rtl_identity_audit.json`: all 20 regenerated RTL files are byte-identical
  to their measured inputs. No measurement fingerprints were rewritten.
- `local_regressions.json`: all ten final regression suites pass.
- `app_smoke.json`: all 18 existing App functional testbenches pass separately.
- `app_capability_matrix.json`: 24 successful generation/syntax checks,
  12 expected rejections, and one empty App directory. These categories do
  not imply functional verification for every App or every possible input.
- `frontend_stress.json`: 100 independent nested-fixture generations succeed
  and emit byte-identical RTL; this does not prove an intermittent assertion
  is fixed.
- `before_event_probe.rpt`, `light_event_probe.rpt`, `vitis_event_probe.rpt`:
  optional simple-pipeline event traces, with times printed in picoseconds.
  They distinguish observed done from the final synchronous-memory write.
- `nested_failed_attempt.json` and `tool_failure_notes.md`: preserve an invalid
  comparison attempt and the known unresolved compiler/tool failures. The
  failed attempt is not part of the 20 successful measurements.

The storage suite checks 14,336 output values across seven existing stress
configurations. Three new boundary fixtures check another 192 return values
and 3,328 read requests across calls without reset, including changing scalar
ports after start acceptance and reading the initial recurrence value from
memory. See [the test descriptions](../../../Light_HLS/Tests/PipelineStorage/README.md).

The five mixed tradeoffs are explicit: complete read/write adds 1 LUT; local
partition adds 7 LUT; FIFO adds 4 FF; PIPO adds 6 FF; partitioned DATAFLOW adds
4 LUT while saving 14 FF. Each reduces cycles. Scalar and complete partition
remain unchanged. These measurements do not establish universal Vitis parity.

The simple pipeline's reported 18 versus 16 cycles includes a difference in
done timing. Its final memory write is one cycle later than Vitis, not two.
See [the implementation and event-semantics notes](../../logs/RTL_BOUNDARY_OPTIMIZATION_20260914.md)
before interpreting completion cycles as intrinsic scheduler quality.

Raw projects, logs, checkpoints, immutable compiler snapshots, failed attempts,
and regeneration audits are retained under
`/tmp/lighthls-boundary-qor-20260914.qyGycC/`. Compilation and synthesis runtimes
from concurrent runs are not speed comparisons. The Vitis combinational
complete-partition case has no registered timing path; missing WNS is not a
measured Fmax. Previous archives remain unchanged.
