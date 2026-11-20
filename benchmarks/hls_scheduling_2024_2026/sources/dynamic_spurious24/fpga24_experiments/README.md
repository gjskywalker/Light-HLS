# Experiment Directory for FPGA '24: Suppressing Spurious Dynamism of Dataflow Circuits Using Latency and Occupancy Balancing

Note: if you are the reviewer responsible for this artifact review, you are
recommended use the VM that we have shared with you instead (everything is
pre-built in this VM you can skip ALL the sophisticated setup). If you don't
have the access to it, please [contact us](mailto:jxu@ethz.ch) as soon as possible.

## Instruction for Using This Artifact

### Obtaining the dependencies

This artifact setup assumes the following installation setup (instruction only
available for Ubuntu 20.04 machines):

- Vivado (2019.1): `/tools/Xilinx/Vivado/2019.1/bin`
- Modelsim (20.1): `/opt/intelFPGA/20.1/modelsim_ase/bin`
- nuXmv-2.0.0: `/opt/nuXmv-2.0.0-Linux/bin`
- gurobi (10.0.3): `/opt/gurobi1003/linux64/bin`

Make sure the the PATH variable contains all the directories above.

This project also requires `python3.10` for setting up a Python virtual
environment.  On Ubuntu, you can use `sudo apt-get install python3.10-full
python3.10-pip` to obtain it.

You need to create an environment script for gurobi as:

This file should be named `/opt/gurobi1003/grbenv.sh`:

```sh
#!/usr/bin/env bash

export GUROBI_HOME=/opt/gurobi1003/linux64
export PATH="${PATH}:${GUROBI_HOME}/bin"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${GUROBI_HOME}/lib"

export GRB_LICENSE_FILE=/opt/gurobi1003/gurobi.lic
```

Which also requires the license to be in a non-standard location `/opt/gurobi1003/gurobi.lic`.

### Building the project

In `dhls-formal/build_utils` there are all the scripts you need to run in order
to build the formal verfication / optimization framework and the Dynamatic HLS
compiler.

Before building Dynamatic (`dhls-formal/etc/dynamatic`), make sure that you
have obtained all the required dependencies specified by the README file in the
Dynamatic directory.

## A Map for the Artifact Evaluation Virtual Machine

This section guides reviewer to inspect the generated log files. It is **not
needed** to understand this section to run the experiments.

### Understand the naming convention

Since the implementation is done before the paper submission, there are some
discrepancies between the naming of the directories and the terminologies on
the paper. In this section we elaborate all of them.

In our manuscript, Table 3 compares the synthesis results of dataflow circuits
optimized using different techniques. 

- `no_opt`: which corresponds to **occupancy** with a cross mark in Table 3
  (i.e. not model checked)
- `occupancy`: which corresponds to **occupancy** with a check mark in Table 3
- `latency`: which corresponds to **latency** with a check mark in Table 3
- `latency_and_occupancy`: which corresponds to **all** with a check mark in
  Table 3

### Software Installtion Locations

| Tool | Location |
| - | --- |
| Vivado | `/tools/Vivado`|
| Modelsim | `/opt/intelFPGA/20.1/modelsim_asm` |
| NuXmv | `/opt/nuXmv-2.0.0/bin/nuXmv` |
| Gurobi | `/opt/gurobi1003` |

### Artifact Locations

| Artifact | Directory |
| -- | ---- |
| Benchmarks | `~/dhls-formal/etc/dynamatic/Regression_test/examples/` |
| Python Library | `~/dhls-formal/dave/dave/` |
| Python Library (our first LP) | `~/dhls-formal/dave/dave/latency_balancing/` |
| Python Library (our second LP) | `~/dhls-formal/dave/dave/buffer_sizing/` |
| Experiment Directory | `~/dhls-formal/fpga24_experiments/` |

### Experiment Directory Organization

- `~/dhls-formal/fpga24_experiments/data/`. This directory stores all the
  generated data. After running the experiments, the results will be parsed
  automatically and stored in this directory.
- `~/dhls-formal/fpga24_experiments/e0_benchmark_overview/`. This directory is
  used for reproducing the results in Table 3.
- `~/dhls-formal/fpga24_experiments/e1_technique_comparison/`. This directory
  is used for reproducing the results in Table 4 and all the colored bar in
  Figure 6.
- `~/dhls-formal/fpga24_experiments/e2_static_hls/`. This directory is used for
  reproducing the red-dashed horizontal line in Figure 6 and reproducing the
  red cross-mark in Figure 7.
- `~/dhls-formal/fpga24_experiments/e4_sweeping/`. This directory is used for
  reproducing the green curve in Figure 7.

Each single experiment run is located in one sub-directory in the locations
above. For instance, imagine you want to inspect the result of `matvec` in
table 3, there are four entries in table 3, therefore, you should be able to
find four directories in
`~/dhls-formal/fpga24_experiments/e1_technique_comparison/` that is named with
`matvec`. From the first row to the last row, the correcponding experiment
directories are:

- `matvec_-_no_opt`: entry with "occupancy" and "cross mark".
- `matvec_-_occupancy_match`: entry with "occupancy" and "check mark".
- `matvec_-_latency_match`: entry with "latency" and "check mark".
- `matvec_-_latency_and_occupancy_match`: entry with "all" and "check mark".

### Single-Experiment Directory Organization

Within each directory for a single experiment run, the following organzation is
used. For instance, within `matvec_-_latency_and_occupancy_match`, we have:

- `hdl`: contains all the generated hdl files that are used for synthesis and
  functional verification.
- `reports`: contains all the Dynamatic related reports or temporary files,
  e.g., intermedate representations in DOT format.
- `reports/smv`: contains the formal verification models and experiment log files.
- `sim`: contains functional simulation results.
- `src`: contains the C-source code for the HLS kernels.
- `synth_rpts`: contains all the synthesis reports generated from Vivado.

### Log Files to Inspect

Assuming you are in a single experiment directory, the following log files are
parsed for generating the figures / tables in our manuscript.

- `reports/latency_matching.log`: LP solver runtime of the first LP model in our
  methodology.
- `reports/buffer_sizing.log`: LP solver runtime of the second LP model in our
  methodology.
- `reports/nuxmv.log`: wall-clock time of the model checking experiments.
- `reports/property.rpt`: the model checking property list.
- `reports/buffers.log`: LP solver runtime of the Dynamatic MILP (using FPGA
  '20 methodology).
- `reports/hls_verify.log`: functional verification results (e.g., used to read
  the number of clock cycles, which is the time when simulation has finished
  divided by 4 ns).


## Running The experiment

### Step 1: reproducing table 3

```sh
$ cd dhls-formal/fpga24_experiments/
$ bash step1_experiments_reproduce_table3.sh
```

After the command finishes, you should be able to view the result of Table 3 in
`dhls-formal/fpga24_experiments/data/e1_technique_comparison.xlsx`

### Step 2: reproducing Figure 6

```sh
$ cd dhls-formal/fpga24_experiments/
$ bash step2_experiments_reproduce_figure6.sh
```

After the command finishes, you should be able to view the result of Figure 6
in `dhls-formal/fpga24_experiments/data/e2_static_hls_no_share.pdf`

### Step 3: reproducing Figure 7

```sh
$ cd dhls-formal/fpga24_experiments/
$ bash step3_experiments_reproduce_figure7.sh
```

After the command finishes, you should be able to view the result of Figure 7
in `dhls-formal/fpga24_experiments/data/sweeping.pdf`

## Appendix 1: Trouble shooting

This section elaborate the potential problems during the experiments, and
fixes.

### Potential Vivado Synthesis Error

It is observed that Vivado may exit abnormally, which crushes the batch script.
Although we did not find the root cause for the problem, but rerun the
synthesis solves the problem. We prepared another script that only redo the
Vivado synthesis part:

```sh
$ cd ~/dhls-formal/fpga24_experiments/
$ bash step1_rerun_synthesis.sh
```

