# Ordinary transaction boundary tests

```bash
python3 Light_HLS/Tests/TransactionEdges/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-transaction-edges
```

Six configurations check constant PHI initialization, argument-dependent PHI
initialization, runtime zero-trip loops, early return, signed constant shifts,
and runtime shift amounts. Each performs 64 pulse-start transactions,
changes scalar ports after start acceptance,
and checks completion, exact output write counts, addresses, and data. The next
transaction starts directly from the completed idle state without extra padding.

An additional 128 transactions per configuration exercise held-start and gapped
traffic, changing the arguments after every `ap_start && ap_ready` sampling edge.
The test interrupts active work with reset, checks that reset suppresses external
memory effects, and checks recovery and final drain. All 1152 completed calls
must pass; the interrupted prefix is not counted as a completed transaction.

Constant/argument PHI and signed/dynamic shift fixtures fuse their empty entry.
All six must fuse their
empty void-return block. Early exits and zero-trip loops must produce no writes.
LLVM verification and independent fixed-width RTL oracles are required. These
tests are bounded simulation checks, not formal equivalence.
