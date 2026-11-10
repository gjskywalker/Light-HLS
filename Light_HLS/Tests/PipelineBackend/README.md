# Pipeline backend regression

This regression validates the first functional loop-pipeline backend. It
covers II=1 execution, a memory-port-limited achieved II, a loop-carried
scalar recurrence, exact request-event semantics for a loop-carried affine
memory dependence,
innermost-loop pipelining, outer-loop pipelining with fully unrolled
descendants, proven-disjoint mixed read/write accesses, and rejection of
multiple PIPELINE directives in one nested loop chain.

Build Light-HLS and run:

```bash
Light_HLS/Tests/PipelineBackend/run_pipeline_tests.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

Every successful case verifies final LLVM bitcode and compares deterministic
expected values with an Icarus simulation of the generated RTL. The II=1 and
memory-limited cases also check the observed interval between RTL writes.
The nested-loop case verifies that surrounding-loop address recurrences remain
symbolic invariants, and the WAR case verifies that a read request rather than
load-data completion constrains a later write request.

The continuous perfect-nest regression checks a 4-by-4 source nest against
16 hardware iterations, while a row-dependent fixture must retain its original
four-iteration inner pipeline. Signed-chain fixtures exercise 256 values per
clock constraint at 100 MHz and 200 MHz, including truncation, sign extension,
arithmetic shifts, and repeated transactions without reset. Failed runs retain
their temporary reports and simulator logs for diagnosis.
