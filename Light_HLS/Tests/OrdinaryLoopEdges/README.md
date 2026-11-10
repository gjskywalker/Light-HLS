# Ordinary outer-loop edge regression

```bash
python3 Light_HLS/Tests/OrdinaryLoopEdges/run_tests.py \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output /tmp/fresh-outer-edge-tests
```

Optional `--baseline-executable /path/to/previous/Light_HLS_Top` runs the same
source, interface configuration, and independent RTL oracle with both compilers.
Every comparison must preserve outputs and not increase measured cycles. The
fixed nested loop must improve. Sources and scalar arguments are randomized
deterministically; 64 calls run without intervening resets per configuration.
Scalar input ports change after acceptance to check argument latching.

Ten fixtures each use one- and two-cycle synchronous memory responses:

| Fixture | Required behavior |
| --- | --- |
| Nested | Merge eligible outer increment/branch and load/initial-PHI edges. |
| Zero/dynamic outer count | Include zero trips and all counts from zero to fifteen. |
| Descending | Preserve old addresses and decrement semantics. |
| Dynamic step | Retain the unsupported nonconstant feedback register. |
| Load live-out | Retain the original read value across the inner loop. |
| Two successors | Select the correct edge-local PHI on either branch. |
| Division live-out | Preserve a load feeding later multicycle arithmetic. |
| Inner pipeline | Optimize the ordinary outer latch only; no read-to-pipeline-PHI bypass. |
| Two port responses | Bypass only the last response; the first must use its register. |
| Load-dependent branch | Retain the capture-to-branch wait and normal PHI sampling. |

The suite checks LLVM IR validity, generated register/wire structure, exact
capture/sample cycles, read/write counts, unique output addresses, and every
output word, including untouched locations. It covers 1,280 calls and 16,492
output writes. FIFO event ordering and clock-enable stalls are checked by the
separate mandatory `FIFOEvents` suite, including the private-buffer case.

The two-response fixture uses constant addresses 0 and 1. An exploratory
`input[i]`/`input[i+8]` version triggers the existing legacy SARE address-analysis
assertion in both the previous and current compilers, before backend scheduling.
That unsupported address-analysis case is not counted as a passing test.

These are bounded simulation and structural checks, not a formal proof over all
possible programs or inputs.
