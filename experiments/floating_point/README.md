# Floating-point characterization

`characterize.py` prepares synthetic scalar IR and extracts the exact bound
functional-unit block from emitted Light-HLS Verilog. It measures independent
dynamic-input operators with Vivado OOC synthesis for the U250 at 100 MHz.
No source kernel controller, context storage, or input mux is included.

```bash
taskset -c 8 python3 experiments/floating_point/characterize.py \
  --formats 16 32 64 --output runs/fp_characterization_new

taskset -c 8 python3 experiments/floating_point/characterize.py \
  --formats 16 32 64 --operations fpext32 fpext64 fptrunc16 fptrunc32 \
  --output runs/fp_conversions_new

python3 experiments/floating_point/install_model.py \
  --runs runs/fp_characterization_new runs/fp_conversions_new \
  --output runs/new_floating_resources_u250.cfg
```

Use a free CPU allowed on the local host. Each run preserves IR, generated RTL,
isolated FU RTL, Tcl/XDC, command receipts, tool logs and utilization reports.
The installer refuses incomplete/mixed-version runs and existing output files.
It reads physical utilization rows: raw hierarchical DSP cell counts include
DSP decomposition cells and are not physical DSP tile counts.

The backend reads the checked-in-format data asset
`Light_HLS/RTL/floating_resources_u250.cfg` through its build-time source path.
Report coverage and missing keys explicitly. Measured individual operators do
not make the sum a measured whole-kernel QoR result; validate a separate held-out
kernel set before using this model for whole-design resource rewards.

Local-only functional helpers are under `Light_HLS/Tests/DAPOII`:
`verify_fp_math.py`, `verify_fp_scheduled.py`, and `verify_fp_fifo.py`.
The FIFO helper includes active-arithmetic stalls and a reset-aborted transaction;
it does not use the synthesis-only hls::stream shim as a software queue oracle.
