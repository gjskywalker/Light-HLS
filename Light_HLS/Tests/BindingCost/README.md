# Cost-aware binding regression

`affinity.cpp` presents four dependent runtime multiplies that can share two
functional units at tight clocks. The backend compares ordinal allocation
with operand-affinity allocation, retaining affinity only when the complete
group's selector-input-bit cost strictly decreases. FU count and schedule are
unchanged. Repeated constant or captured-argument inputs can bypass selectors.

```sh
python3 Light_HLS/Tests/BindingCost/run_tests.py /absolute/path/Light_HLS_Top \
  --output /tmp/new-binding-tests
taskset -c 0 python3 Light_HLS/Tests/BindingCost/compare_synthesis.py \
  --before /absolute/path/old/Light_HLS_Top --after /absolute/path/new/Light_HLS_Top \
  --output /tmp/new-binding-synthesis
```

The local test runs 256 calls at each of 100, 150, 200, 250, and 333 MHz. It
changes inputs immediately after acceptance and checks registered return hold.
An uncovered 500 MHz characterization request must fail cleanly without RTL.
The synthesis comparison uses the same source, testbench, interface, U250 part,
4 ns clock, and common Vivado OOC flow for both executables.

`selector_input_bits_v1` is a deterministic proxy, not an estimated LUT count
or a globally optimal binding objective. Physical gains and regressions must
be measured separately; do not infer DSP savings from selector cost alone.
