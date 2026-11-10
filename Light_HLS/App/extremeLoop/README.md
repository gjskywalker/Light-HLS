# Backend ap_memory smoke test

This design exercises two independent single-port `ap_memory` interfaces,
synchronous one-cycle reads, writes, pointer-to-integer address induction,
sequential loops, scheduling, binding, and `ap_ctrl_hs` control.

The frontend recovers the declared source depths of `A[6]` and `B[8]`. The
backend records both as exact frontend depths and emits a three-bit element
address for each port.

`extremeLoop_tb.v` provides self-checking synchronous memory models for the
generated `A_*` and `B_*` ports. It verifies all updated array elements with
Icarus Verilog.
