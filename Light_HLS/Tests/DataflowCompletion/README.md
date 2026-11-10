# DATAFLOW completion-frontier regression

The producer emits all 16 FIFO tokens, then performs 64 public memory writes.
The consumer can therefore finish before the producer. Top-level completion
must cover both tasks, not just the graph sink. This also checks that removing
the wrapper's extra completion register does not announce completion early.

```sh
python3 Light_HLS/Tests/DataflowCompletion/run_tests.py /absolute/path/Light_HLS_Top \
  --output /tmp/new-completion-tests
```

The test runs 64 transactions each with natural execution and injected
producer stalls. It checks 10,240 total writes, changed post-acceptance scalar
inputs, exact output addresses/data, and completion after all effects.

An acyclic producer need not have its own completion credit only if a balanced,
non-initialized outgoing channel proves that consumer completion covers it:
PIPO ownership transfers after producer completion, or the exact FIFO timing
places the producer's final token on its final enabled edge. Otherwise the
producer is tracked too. Cyclic graphs conservatively track all tasks. RTL
generation and performance analysis use the same completion-task selection.
