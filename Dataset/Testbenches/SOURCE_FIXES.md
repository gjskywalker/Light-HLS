# Testbench source contracts

This directory contains the merged 80-case corpus. The original sources under
`Dataset/Cross_Validation/0` are unchanged. Exactly eight copies were repaired;
`source_fixes.cfg` registers their independently validated SHA-256 hashes.
Unknown edits and original unsafe versions are not exempted from verifier checks.

| Cases | Source/API repair | Independent acceptance |
| --- | --- | --- |
| random5, random23 | Initial FFT span 128, eight stages | 32 butterfly differential tests per format |
| random30 | Guard static line-buffer reads during drain columns | Four full-size boundary/drain fixtures |
| random35 | Increase input extents to 8208; retain existing windows | Eight full-size dot-product checks |
| random57, random58 | Add expanded-key array inputs and byte validation; random57 validates round 0..14 | 256 GF(2^8) differential tests each; CBMC equivalence and safety checks |
| random66 | Complete opposite-sign integer software binary64 addition | 46 original vectors, 256 edge pairs, 50,000 native-FP64 differential tests |
| random68 | Correct old/new bank selection; validate log_width 3..6 | 64 boundary, wrap and invalid-width tests |

Native acceptance used ASan/UBSan with no recovery. Evidence is retained under
`runs/testbench_source_fixes_20261113/`; it establishes source-repair acceptance,
not RTL correctness or calibrated QoR. The extracted random30 kernel retains
its existing zero-gradient calculation; it is not a complete optical-flow kernel.

The registry and this document were restored from the preserved acceptance
receipts during compiler regression on 2026-11-13. All eight current source
hashes matched those receipts; no source was changed during restoration.
