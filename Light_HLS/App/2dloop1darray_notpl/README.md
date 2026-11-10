# Pragma-free pointer-array correctness test

This existing integer kernel contains an outer loop, an inner loop, a branch,
and one pointer argument. The RTL backend infers a conservative minimum depth
of 58 elements from the transformed IR.

`2dloop1darray_notpl_ref.cpp` runs the original source with deterministic
initialization. `2dloop1darray_notpl_tb.v` implements a one-cycle synchronous
`ap_memory` model and checks every final element. The regression verifies the
final bitcode with LLVM's verifier, then compares every source and RTL `RESULT`
record.

The final Light-HLS memory IR is not executed natively as a correctness oracle.
Its lowered pointer arithmetic uses element-address increments for backend
analysis, while a native LLVM target interprets the same increments as bytes.
