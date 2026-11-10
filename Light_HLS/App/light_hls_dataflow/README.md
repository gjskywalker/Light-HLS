# Light-HLS DATAFLOW examples

These examples exercise the currently implemented task-level DATAFLOW backend:

- `vector_stream_dataflow.cc` uses two scalar FIFO channels with different
  depths;
- `vector_stream_dataflow_depth1.cc` forces full/empty backpressure on every
  transfer;
- `vector_array_dataflow.cc` uses two ping-pong local-array channels;
- `vector_stream_array_dataflow.cc` uses a two-dimensional array containing six
  independent FIFO instances;
- `vector_stream_persistent_dataflow.cc` submits adjacent transactions to the
  persistent controller;
- `vector_stream_pipeline_dataflow.cc` pipelines the loops inside each task;
- `vector_external_partition_dataflow.cc`,
  `vector_array_partition_dataflow.cc`, and
  `vector_complete_partition_dataflow.cc` exercise external and internal
  banked memories;
- `vector_pipeline_partition_dataflow.cc` combines task PIPELINE and cyclic
  ARRAY_PARTITION in one DATAFLOW region;
- `vector_partial_pipo_dataflow.cc` transfers an exact prefix of a PIPO;
- `vector_broadcast_dataflow.cc` broadcasts every produced item to two
  consumers;
- `initialized_cycle_dataflow.cc` breaks a directed startup cycle with one
  zero-valued initial token;
- `public_fifo_dataflow.cc` exposes scalar top-level `ap_fifo` ports and tests
  output backpressure;
- `conditional_fifo_dataflow.cc` exercises a runtime path that satisfies a
  conditional FIFO's rate contract while declining to claim a static graph-II
  result.

The backend requires direct, unconditional task calls and exclusive ownership
of each external memory by one task. FIFO channels support one producer and one
or more broadcast consumers; multi-producer fan-in is rejected. Local-array
channels use two transaction buffers with producer commit and consumer release
handshakes. The physical layout may be unpartitioned, cyclic, block, or
complete, and exact balanced transfers may use less than the full declared
capacity. The examples use native integer datapaths, `ap_memory` external
arrays, and scalar `ap_fifo` stream ports.

Public stream arguments currently require the Light-HLS STREAM extension
`bitwidth=<N>`, for example:

```cpp
#pragma HLS STREAM variable=input_stream depth=4 type=fifo bitwidth=32
```

An initialized channel uses `initial_tokens=<N>`. Initial payload values are
zero in the current implementation. Top-level stream arrays, dynamic PIPO
rates, and arbitrary-width `ap_int`/`_BitInt` ABIs are outside the supported
boundary.
