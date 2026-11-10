# Ordinary-loop bank analysis regression

These fixtures contain no PIPELINE pragmas. The suite checks bank proofs and
per-bank request reservations before simulating the generated RTL. Run it with:

```bash
bash Light_HLS/Tests/OrdinaryBankAnalysis/run_tests.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

An optional `--output /absolute/fresh/directory` retains a named run. By default,
artifacts are retained in a unique `/tmp/light-hls-ordinary-banks.*` directory.
The suite is also registered in `Tests/run_regressions.py`.

| Fixture | Required analysis/scheduling behavior |
| --- | --- |
| `cyclic_parallel` | Four distinct cyclic banks issue in the same cycle. |
| `block_parallel` | Four distinct block banks issue in the same cycle. |
| `same_bank` | Two reads to bank zero occupy different request cycles. |
| `dynamic_offset` | A scalar-dependent lane retains all-bank coverage. |
| `dynamic_count` | An unknown trip count, bounded by `count & 7`, retains all-bank coverage. |
| `unsigned_wrap` | Eight-bit truncation across 255 preserves the actual factor-three bank set. |
| `signed_extension` | A sign change, lowered to arithmetic shift by strength reduction, preserves the actual factor-three bank set. |
| `descending` | Negative induction steps retain a proven static bank. |
| `mixed_hazards` | Dynamic reads and static writes retain RAW/WAR/WAW ordering. |
| `nested_fallback` | Nested ordinary loops retain conservative coverage. |

Each fixture runs with one- and two-cycle synchronous input-memory response
latencies and 64 transactions without reset. Deterministic xorshift input data
varies between calls. Scalar ports change immediately after start acceptance.
The testbench checks output values, duplicate/lost writes, read/write request
counts, and all 256 input-memory words after each call. The mixed-hazard case
exercises every runtime lane, including lanes aliasing each static store.
The dynamic-count case includes zero-iteration calls and checks untouched
output locations. In total, 20 configurations check 1,280 calls and 9,664
written outputs, in addition to memory and event checks.

This is deterministic simulation coverage, not formal equivalence over all
inputs. Unsupported expressions must retain conservative bank sets; the suite
does not authorize speculative banking or elimination of memory dependencies.
