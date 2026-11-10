# Array-partition backend regression

This regression validates array partitioning as a first-class backend pass. It
includes read-only and read/write complete scalar interfaces, and covers
cyclic, block, complete, a complete-partition II=1 pipeline, multidimensional
`dim=0`, partial
multidimensional cyclic, and local-array layouts. The checks include recovered
source dimensions, bank count and depth,
static and dynamic possible-bank sets, bank-aware pipeline II, Vitis-compatible
bank port names, scalar ports for fully partitioned external arrays, and local
bank storage.

Build Light-HLS and run:

```bash
Light_HLS/Tests/ArrayPartitionBackend/run_array_partition_tests.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

Every positive case verifies final LLVM bitcode, parses all relevant JSON
reports, checks the binding metadata, compiles the generated Verilog with
Icarus, and compares every output against deterministic expected values. The
negative case verifies rejection of an out-of-range partition dimension.

The U250 cyclic reference also matches Vitis HLS 2025.2 at achieved II=1 and at
the native top-level interface: four read-only `input_<bank>` `ap_memory`
interfaces with two-bit local addresses and one unpartitioned write-only output
interface. The complete read-only and read/write cases also match Vitis native
`ap_none` and `ap_ovld` scalar bank naming.
