# Pragma-free multi-memory correctness test

The existing `compute` kernel contains two sequential 800-iteration loops and
three external integer arrays. It exercises multiplication, three independent
`ap_memory` objects, writes in the first loop, and read-after-write behavior in
the second loop without relying on HLS pragmas.

`sgd_ref.cpp` emits the source reference trace. `sgd_tb.v` uses three one-cycle
synchronous memory models, checks all gradient and parameter values, and emits
the same trace for source-to-RTL regression comparison. The final bitcode is
checked with LLVM's verifier but is not executed natively because Light-HLS's
lowered memory IR uses element-address increments for backend analysis.
