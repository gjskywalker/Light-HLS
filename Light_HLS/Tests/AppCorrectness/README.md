# Supported App correctness regression

This regression tests existing pragma-free integer applications that stay
inside the current RTL backend feature boundary. It checks:

- successful scheduling, binding, and RTL generation;
- structurally valid final LLVM bitcode;
- synthesizable SystemVerilog accepted by Icarus Verilog;
- cycle-accurate one-cycle `ap_memory` interaction;
- exact agreement between the source reference and final RTL memory contents;
- expected memory depth metadata and an empty pragma configuration.

Run it from any directory:

```bash
Light_HLS/Tests/AppCorrectness/run_supported_apps.sh \
  /path/to/Light_HLS_Top
```

## Why memory bitcode is not run natively

Light-HLS's final lowered memory IR represents pointer increments in elements
so the scheduling and RTL backend can treat addresses as element indices. A
native LLVM target instead applies byte-address semantics to the same
`ptrtoint`/`inttoptr` sequence. Native execution of this backend-oriented IR
therefore is not a valid functional oracle for memory kernels.

The regression still asks LLVM to verify the final bitcode. Functional
correctness is established by comparing the original source against the RTL
using identical deterministic input data. Scalar-only backend tests retain the
stronger source/final-IR/RTL comparison where native IR execution is valid.

## Current cases

- `2dloop1darray_notpl`: nested loops, a branch, and inferred external-memory
  depth;
- `spam-filter`: two long loops, three exact-depth external memories,
  multiplication, and read-after-write behavior.

Pipeline, array partition, dataflow, floating point, streams, and complete AXI
interfaces are intentionally outside this regression until their backend
implementations are available.
