# Preserved unsuccessful attempts

These attempts are not successful QoR measurements. They are retained so that
passing retries do not erase evidence of tool or compiler instability.

## Linker abort

During development, `ld.lld` aborted with `invalid symbol kind` and an
unreachable assertion in `lld/ELF/Symbols.cpp:156`. The traceback includes
LLVM's parallel thread-pool implementation. Retrying the same build pinned to
CPU 0 succeeded without a source change. The root cause is not established;
CPU pinning is not a demonstrated fix.

Logs are `build-v2b.log` and `build-v2c.log` under
`/tmp/lighthls-boundary-qor-20260914.qyGycC/`.

## Legacy estimator assertion

One `regressions-final` run stopped while evaluating the nested partitioned
DATAFLOW fixture, before the RTL backend. The assertion was:

```text
llvm::dyn_cast: To = llvm::SCEVConstant; From = const llvm::SCEV
Assertion `detail::isPresent(Val) && "dyn_cast on a non-existent value"' failed.
```

Original artifacts remain at
`/tmp/light-hls-dataflow-test.Btsygh/nested_pipeline_partition/`.
The failed and successful `top_output_PM4.bc` disassemblies differ only in
the module filename and debug compilation directory. No backend source change
was made in response to this assertion.

Two subsequent complete ten-suite runs passed with the same final compiler.
Ten additional isolated generations of the nested fixture also passed: five
using the previous checkpoint executable and five using the final executable.
Those runs are in `frontend-recheck/` under the working root. They do not prove
that the assertion is fixed or establish its cause. The final archived local
regression report is `regressions-final-v3/local_regressions.json` and includes
the three new recurrence boundary fixtures.

A further 100 independent generations with the final compiler all succeeded
and produced byte-identical RTL. `frontend_stress.json` records that check;
raw outputs are retained in `frontend-stress/` under the working root. This is
additional non-reproduction evidence, not proof that the assertion is fixed.

The same assertion subsequently recurred during the initial nested U250
comparison on CPU 2. Its invalid comparison report and all inputs/logs are
preserved under `u250-final/dataflow_nested.failed.1/`; that failed attempt is
excluded from successful QoR measurements. A single GDB run and 30 repeated
GDB runs pinned to CPU 2 then exited normally, so CPU affinity alone does not
reproduce the assertion. The debugger logs are `gdb-cpu2-native.log` and
`gdb-cpu2-repeat.log`. The complete comparison was restarted with identical
compiler and source/testbench fingerprints in a fresh output directory.

A reproducible failure should next be captured with a debugger or a
sanitizer-enabled build of the legacy estimator; replacing a null SCEV with a
guessed constant would not be a safe optimization.
