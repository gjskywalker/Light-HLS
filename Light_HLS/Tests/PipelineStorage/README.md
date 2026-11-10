# Response and pipeline-storage regressions

Run `run_storage_tests.sh /path/to/Light_HLS_Top`, or select `PipelineStorage`
with the unified regression driver. Successful temporary outputs are removed;
failed runs are preserved and their location is printed.

The suite checks 64 transactions without reset for each of the following:

- Three pointer-dependent loads, with one- and two-cycle external RAM reads.
  Early responses have multiple later uses and must not be forwarded. Values
  with short lifetimes use scalar registers while longer ones retain a ring.
- The same pointer-dependent accesses on four cyclic banks, with one- and
  two-cycle reads. Data-dependent bank selectors must remain valid through
  response capture; these responses retain explicit capture.
- Two dependent reads at II=2, requiring a two-slot value buffer inside a
  four-slot controller ring. The test covers both buffer and controller wraps.
- Two simultaneous reads on explicitly configured dual-port memory. Both
  responses must be forwarded and the II=1 controller must use static tags.
  `dual_port.cfg` configures the Light-HLS memory port count explicitly.

A depth-one FIFO graph additionally checks 32 transactions with pipelined
producer/compute tasks and a slower non-pipelined consumer. External RAM
requests must stop whenever the owning task stalls. Each case checks all
output values and the number of writes, not just `ap_done`.
The seven configurations check 14,336 output values in total.

Three additional recurrence fixtures check constant, runtime-argument, and
memory-loaded initial values over 64 calls each, without reset. The testbench
changes scalar input ports
immediately after start acceptance, verifies the sampled arguments and all
memory requests per call, and checks that the return port does not change
before completion. Constant initialization is folded into start acceptance;
runtime initialization retains the separate edge, and memory initialization
must complete its preheader read. All three fixtures require the
retirement-only recurrence result to share the feedback register. Together
they check another 192 return values and 3,328 read requests.

`run_app_smoke.py --light-hls-top ... --output ...` reuses the 18 existing
pipeline, array-partition, and DATAFLOW App testbenches. This independent check
does not replace the exact scheduling assertions or the Vitis/Vivado fair
comparison harness.
