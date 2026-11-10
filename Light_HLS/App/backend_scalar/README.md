# Backend scalar smoke test

This design exercises the initial RTL backend scope: integer scalar ports,
conditional control flow, a sequential loop, PHI lowering, scheduling, binding,
and the `ap_ctrl_hs` interface. `backend_scalar_tb.v` is an Icarus Verilog
self-checking testbench for the generated `rtl/top.v`.

`backend_scalar_ref.c` is linked once with the source implementation and once
with Light-HLS's final optimized bitcode. The RTL regression compares both
traces with the self-checking Icarus simulation, providing a source/IR/RTL
equivalence baseline for the supported scalar subset.
