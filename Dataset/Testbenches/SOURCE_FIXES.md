# Testbench source contracts

This directory merges the 70 training and 10 test sources from
`Dataset/Cross_Validation/0`. Those original files are unchanged. Exactly eight
copies were repaired; `source_fixes.cfg` registers their validated SHA-256 hashes.
Unknown edits and original unsafe versions are not exempt from optional verifier
checks. The other 72 merged copies are byte-identical to their originals.

| Cases | Source/API repair | Independent source acceptance |
| --- | --- | --- |
| random5, random23 | FFT initial span 128, eight stages | 32 butterfly differential tests per format |
| random30 | Guard static line-buffer reads in drain columns | Four full-size boundary/drain fixtures |
| random35 | Input extents 8208, retaining existing windows | Eight full-size dot-product checks |
| random57, random58 | Expanded-key input and byte validation; random57 round 0..14 | 256 GF(2^8) differential tests each and CBMC equivalence/safety checks |
| random66 | Complete opposite-sign integer software binary64 addition | 46 original vectors, 256 edge pairs, 50,000 native-FP64 differential tests |
| random68 | Correct old/new bank selection; log_width 3..6 | 64 boundary, wrap and invalid-width tests |

Native acceptance used ASan/UBSan with no recovery. Local evidence is retained
under `runs/testbench_source_fixes_20261113/` and is intentionally not committed.
Source acceptance is not proof of RTL correctness or calibrated QoR. The
extracted random30 kernel retains its existing zero-gradient calculation; it
is not a complete optical-flow implementation.

The merged corpus is used by the default search-to-RTL experiment. Each design
is searched independently; the original split does not establish held-out
generalization. Functional simulation is optional and is not part of the
default RL/estimator/RTL-generation workflow.
