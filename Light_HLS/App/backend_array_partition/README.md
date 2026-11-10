# Array Partition Backend Cases

These cases exercise the independent Light-HLS array-partition backend:

- `backend_array_partition_cyclic.cpp`: one-dimensional cyclic banking.
- `backend_array_partition_block.cpp`: one-dimensional block banking.
- `backend_array_partition_complete.cpp`: complete banking of one dimension.
- `backend_array_partition_complete_rw.cpp`: direction-aware scalar input,
  output, and valid ports for a completely partitioned read/write array.
- `backend_array_partition_multidim.cpp`: `dim=0` complete banking of every
  dimension in a multidimensional array.
- `backend_array_partition_multidim_cyclic.cpp`: partial cyclic banking of the
  inner dimension while retaining a bank-local multidimensional address.
- `backend_array_partition_local.cpp`: cyclic banking of a local array.
- `backend_array_partition_pipeline_complete.cpp`: an II=1 pipeline with a
  dynamic index into a completely partitioned external array.

The regression in `Tests/ArrayPartitionBackend` checks the resolved bank
layout, bank-aware scheduling and binding, generated RTL syntax, and RTL
functionality.
