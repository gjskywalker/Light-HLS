# Ordinary self-backedge bypass tests

```bash
python3 Light_HLS/Tests/OrdinaryBackedge/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-backedge-tests
```

Add `--baseline-executable /path/to/prior/Light_HLS_Top` for same-source,
same-testbench cycle comparisons. The supported baseline is the preceding
ordinary-feedback checkpoint. Its narrow fixture emits invalid cross-block
integer IR, so that fixture is explicitly recorded as non-comparable, not as a
successful baseline execution. The other fixtures must not increase cycles;
the fixed nested recurrence must strictly improve.

The suite covers fixed and runtime-bounded nested recurrences, simultaneous PHIs
that depend on each other's old values, load-dependent updates, a delayed
transition, PIPELINE exclusion, narrow cross-block live-outs, and a multicycle
divider exclusion, and an old-PHI live-out that prohibits storage sharing.
All nine variants run with one- and two-cycle input-memory reads (18 configurations).
Every configuration performs 64 reset-free calls, changes scalar ports after
start acceptance, checks 1,024 output words, and verifies exact read/write counts.
The oracle uses fixed-width unsigned arithmetic and deterministic input data.

Structural checks require each bypass to retain its result value and feed
the PHI from a bound operator output exactly on that operator's issue cycle.
When the old PHI has only block-local non-PHI users and the update has a real
cross-block live-out, the update and PHI may
share one register. The ordinary issue write updates that register on both
self-edges and exits; a live-out of the old PHI must retain separate storage.
The pipeline and multicycle variants require no bypasses. In the delayed case,
the early XOR update must still be sampled through its retained register;
only the later memory-dependent update may qualify. LLVM verification checks
the final bitcode. These are bounded simulations, not formal equivalence.

The load-dependent fixture uses `input[j]`. An exploratory masked-index variant
`input[(i+j)&15]` was rejected by the existing depth analysis with inferred depth
23 despite its mask. That pre-existing inference limitation is not fixed by
this scheduling change or counted as a passing masked-address test.
