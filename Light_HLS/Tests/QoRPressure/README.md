# DSP, BRAM, and transaction-pressure tests

These are new benchmarks, not rewritten versions of the App comparison cases.
All arithmetic is unsigned integer arithmetic. Each design processes 256 words.

| Design | Pressure exercised |
| --- | --- |
| `dsp_map` | Runtime 17-bit multiply/add and one pipelined loop |
| `bram_permute` | Explicit local single-port BRAM, wrapped addresses, adjacent pipelines |
| `bram_banked` | Two BRAM banks and potentially conflicting dynamic reads |
| `dsp_dataflow` | Three pipelined tasks, depth-four FIFOs, DSP datapath |

Run from the repository root with an already built, frozen executable:

```sh
python3 Light_HLS/Tests/QoRPressure/run_tests.py /absolute/path/Light_HLS_Top \
  --output /tmp/new-pressure-local --freq-mhz 100 --freq-mhz 250
taskset -c 0 python3 Light_HLS/Tests/QoRPressure/run_tests.py /absolute/path/Light_HLS_Top \
  --output /tmp/new-pressure-u250 --freq-mhz 100 --freq-mhz 250 --compare
```

Output directories must be absent or empty. `--case` and `--freq-mhz` can be
repeated. The default is all four designs at 100 MHz. The comparison runs
Vitis HLS, both native and normalized-wrapper Vivado OOC synthesis, and RTL
simulation. Both generators must match the native C++ reference trace. DSP
cases must actually use DSPs; BRAM cases must actually use BRAM tiles.

The native reference compiles the same kernel source. `software/hls_stream.h`
is a software-only queue adapter; it is never passed to either synthesis flow.
The existing Light-HLS synthesis stream shim is unchanged.

## Event contract

The sequential testbench checks eight transactions with changing inputs and
coefficients, exactly 256 unique writes and complete input-address coverage,
and no later effects. Light-HLS must issue exactly 256 reads. Vitis BRAM tests
allow one repeated, idempotent ordinary RAM read while exiting a loop; FIFO
events and writes are not relaxed. It records first launch, first read/write, last write commit,
and first observed `ap_done`. The `accepted` timestamp denotes the initial
start/launch edge, not the later Vitis `ap_ready` acknowledgement.

The Vitis adapter holds start until a sampled ready edge, then releases it
while the pipeline drains. Holding start until done can accidentally begin a
second transaction; pulsing it for one cycle can stall a Vitis loop pipeline.
The adapter does not mask duplicate or missing writes. Unused memory directions
are exposed only where the generated Vitis interface requires them.

`completion_signal_cycles` and `final_commit_cycles` are different metrics.
Vitis can assert done before the final memory sampling edge. Acceptance
intervals from this paced testbench are not the minimum transaction II.

The Light-only queued test additionally checks 32 transactions, two outstanding
requests, 8,192 writes, changed scalar arguments, and same-edge completion plus
acceptance. It runs naturally and with injected store-task stalls. These tests
validate Light-HLS queue semantics; they do not claim a Vitis throughput win.

## Simulator compatibility and preserved synthesis

Local Light-HLS regressions use Icarus. The DATAFLOW comparison uses Vivado
XSim for both unmodified RTL implementations: Icarus leaves a Vitis generated
`always @*` backpressure expression unknown when its FIFO full flag is already
initialized at time zero and never transitions. XSim executes this case
correctly. Its `xvlog --relax` option accepts existing Light-HLS forward wire
references; it does not rewrite the RTL. XSim requires working local IPC and
may need to run outside a restricted execution sandbox.

`FairComparison/revalidate_u250_comparison.py` can rerun functional checks with
a corrected testbench or simulator against existing synthesis artifacts. It
creates a new report, fingerprints the unchanged RTL, wrappers, metrics and
logs, and explicitly marks synthesis reuse. It never claims a fresh synthesis
run or invents missing runtimes. The original failed comparison is preserved.
The standard comparison also rejects source, testbench, compiler, or config
files changed while the run was in progress.

## Scope

The tests do not add floating point, AXI, external streams, or general nested
pipeline support. The banked fixture intentionally exposes conservative bank
conflict scheduling; a successful test does not imply optimal II. Synthesis
timing is not place-and-route timing. Frequency plans alone are not measured
QoR results. Keep original measurement fingerprints when checking a newer
compiler against an already synthesized, byte-identical RTL file.
