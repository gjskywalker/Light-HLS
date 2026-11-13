# Berkeley HardFloat Release 1

Vendored from https://www.jhauser.us/arithmetic/HardFloat-1.zip (Release 1, July 2019).
Archive SHA-256: `6b3757c9fbfa2230c6a2b84605e39372cb589dd7500e979c4f0b8ecc8a03b14b`.
Upstream license: BSD-3-Clause, retained in `COPYING.txt` and source headers.
Only arithmetic RTL and the RISC-V specialization are included. No upstream
tests or binaries are required at runtime. Most source files are unchanged
apart from text-file trailing newlines. Local modification (2026-11-11):
`divSqrtRecFN_small.v` adds a synchronous `clockEnable` port to both modules
and guards all iterative state updates with it. Reset remains unconditional.
This freezes the arithmetic state together with a stalled DATAFLOW task;
no combinationally gated clock is generated. Original copyright and license
notices are retained. With clockEnable tied high the original behavior is preserved.

LightHLS expands includes into the emitted Verilog so simulation and synthesis
do not depend on external include paths. Arithmetic uses round-to-nearest,
ties-to-even; float-to-integer conversions use round-toward-zero. Exception
flags are not exposed through the current C interface. NaN payload identity
is not promised. FP-to-integer inputs outside the LLVM-defined representable
range remain undefined and must not be used as correctness references.
