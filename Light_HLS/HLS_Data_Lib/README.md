# LightHLS instruction data libraries

Instruction timing and resource tables are board-specific:

```
HLS_Data_Lib/
  boards/
    artix7_a12t/
    u250/
  tools/
    generate_vitis_library.py
```

The original data was characterized for part `xa7a12tcsg325-1q`. It is
available as `artix7_a12t` and remains the default for backward compatibility.
The `u250` library targets Alveo U250 part `xcu250-figd2104-2L-e`.
Top-level operation paths are compatibility symlinks to the default library,
so existing configurations that point directly at `HLS_Data_Lib/` keep
working.

Each operation table contains eleven whitespace-separated fields:

1. input A bit width (`-1` when independent of width)
2. input B bit width
3. result bit width
4. target clock period in ns
5. DSP count
6. FF count
7. LUT count
8. operation latency in cycles (`-2` is LightHLS's load-scheduling sentinel)
9. residual operation delay in ns
10. initiation interval
11. Vitis HLS core name

LightHLS chooses a library with `--board`, `HI_HLS_BOARD`, or the
`FPGA_board` config setting. The `boards/default` symlink names the fallback
library.

## Regenerating a Vitis library

Keep the Vitis work directory outside the repository:

```bash
python3 HLS_Data_Lib/tools/generate_vitis_library.py \
  --board u250 \
  --part xcu250-figd2104-2L-e \
  --output /tmp/HLS_Data_Lib_u250 \
  --work-dir /tmp/HLS_Data_Lib_u250_work \
  --jobs 2
```

The generator synthesizes independent microkernels at the 15 clock periods
used by LightHLS and extracts timing and resource data from the generated
verbose reports. Integer characterization covers the native 8, 16, 32, and
64-bit widths. The `dmuladd` probe pipelines `hls::fma` at `II=1`.
Load and store rows retain the scheduler's `-2` and `0` latency conventions
while taking their RAM delay from the selected part's Vitis report.
LightHLS uses its existing nearest-width lookup for other integer widths.
