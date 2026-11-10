# Ordinary-loop feedback regression

```bash
python3 Light_HLS/Tests/OrdinaryFeedback/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-ordinary-feedback
```

This suite checks constant-step feedback wire selection without changing the
held PHI before the last memory-use edge. Each selected update must be a wire,
its PHI must remain a register, and the self-backedge must sample that wire.
LLVM verification and an independent Icarus oracle check the generated design.

Eight source variants run at both one- and two-cycle input-memory latency:
eight-bit address wraparound, descending addresses, bounded runtime trip count
(including zero), runtime step (including zero), data-dependent early exit,
an explicit pipeline, load-dependent step, and a sign-extended return value.
One additional ordinary-loop run uses the tightest characterized U250 library
period, 3 ns. All 17 configurations execute 64 calls without reset: 1,088 calls
in total. Scalar arguments change immediately after start acceptance. The
oracle checks every output, the return value, exact read/write counts, duplicate
writes, and unknown addresses. Randomness is deterministic and reproducible.

The address recurrence itself is not necessarily selected: cross-block uses,
casts around a narrowed update, variable steps, and pipelines retain their
existing storage. Ordinary induction-counter updates exercise the positive
selection path. The pipeline cases require zero ordinary feedback wires.
This is bounded simulation, not formal equivalence or a timing-closure claim.

Runtime counts are explicitly masked to their tested range, and output indices
are masked to the declared array extent. Unbounded trip-count depth inference
and unsupported `llvm.umin`/`llvm.umax` lowering are not enabled by this change.
The generator removes inactive PIPELINE pragmas before compiling ordinary
variants because directive discovery currently precedes preprocessing.

The wrap fixture also guards a pre-existing width-reduction bug: narrowing an
arithmetic value must preserve the PHI operand type and the function return
ABI. PHI conversions belong on their incoming CFG edge, not in the PHI block.
Unsigned and signed-return simulations check the corresponding extension bits.
The out-of-library 0.1 ns exploratory constraint is not a supported regression;
the legacy timing-library lookup still asserts for that unsupported period.
