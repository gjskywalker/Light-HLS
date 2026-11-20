# Artifact for ASPLOS '25

Here are the code, benchmark, and artifact evaluation instructions for our
ASPLOS '25 paper: CRUSH: A Credit-Based Approach for Functional Unit Sharing in
Dynamically Scheduled HLS.

## 1. Repository map

This section describes the list of files:
```
.
├── benchmarks                            # Benchmarks for Table 2
├── benchmarks-fast-token                 # Benchmarks for Table 3
├── benchmarks-mlir                       # Benchmark (gesummv) for table 1
├── data                                  # Generated figures and tables
├── legacy-dynamatic                      # Baseline "in-order" in Table 2
├── legacy-dynamatic-fast-token           # Baseline "fast-token" in Table 3
├── manually_generated_dots               # One fixed example from "fast-token"
├── pyhash                                # Python implementation of CRUSH
├── sharing-overhead-comparison           # Reproduce Figure 9 and 10
├── vivado_fpu_ip_wrappers                # FPU wrapper for table 1
├── build_project.sh                      # Script for automatically compile the dependencies
├── define.py                             # Global definitions
├── Dockerfile                            # Docker image building script
├── fix_dot.py                            # Fixing some bugs in "in-order" artifact
├── flow_compile_fast.sh                  # C-to-dataflow compile flow (Table 3)
├── flow_compile_legacy.sh                # C-to-dataflow compile flow (Table 2)
├── flow_sharing.py                       # Script for our sharing flow
├── flow_simulate.sh                      # Script for running simulation
├── flow_synthesize.sh                    # Script for running synthesis
├── install_config.txt                    # Vivado installation options
├── manual_e4_fast_token_shared_gsumif.sh # Evaluating manually fixed benchmark (CRUSH)
├── manual_e5_fast_token_gsumif.sh        # Evaluating manually fixed benchmark (in-order)
├── table_crush_vs_fast.py                # Generate table 3
├── table_crush_vs_inorder_vs_naive.py    # Generate table 2
├── plot_crush_vs_fast.py                 # Generate Figure 11
├── plot_crush_vs_inorder.py              # Generate Figure 8
├── plot_crush_vs_naive.py                # Generate Figure 7
├── README.md                             # This file
├── run_e1.py                             # Batch script for CRUSH in Table 2
├── run_e2.py                             # Batch script for In-order in Table 2
├── run_e3.py                             # Batch script for Naive in Table 2
├── run_e4.py                             # Batch script for CRUSH in Table 3
├── run_e5.py                             # Batch script for In-order in Table 2
├── run_vivado2019.py                     # Driver script for Vivado 
└── utils.py                              # Python utilities
```

## 2. Prerequisites

This section describes the prerequisites for running this artifact (e.g.,
system requirements, software packages, etc.).

### System requirement

This artifact depends on some disk and memory demanding software packages
(LLVM, Vivado, etc.). It is recommened to do the experiment on a linux machine
with: 
- At least 32 GB memory.
- At least 180GB free disk space.
- Docker installed.

We provide a docker file that can automatically install all the required
packages in a push-button fashion. Yet, we kindly ask to to download all the
proprietary dependency packages since they cannot be on a public archive. All
the software can be downloaded for free and the Gurobi solver offers academic
license (requires internet connection from an academic institute).

We further constrain the version of the software for maximum experiment
reproducibility.

### Getting the Vivado Installer

Download [Vivado 2019.1 installer](https://www.xilinx.com/support/download/index.html/content/xilinx/en/downloadNav/vivado-design-tools/archive.html).

Click 2019.1. You can find it under "Vivado HLx 2019.1: All OS installer
Single-File Download".

You will download a file called `Xilinx_Vivado_SDK_2019.1_XXXX_XXXX.tar.gz`.
**Put in under this directory.**

### Getting the Modelsim Installer

Download [Intel Modelsim 20.1](https://www.intel.com/content/www/us/en/software-kit/750637/modelsim-intel-fpgas-standard-edition-software-version-20-1.html).

You will download a file called `ModelSimSetup-20.1.0.711-linux.run`.
**Put it under this directory.**

### Getting Gurobi (11.0.3) and WLS License

Download [Gurobi optimizer for
Linux](https://www.gurobi.com/downloads/gurobi-software/) (log in required).
**Please download version 11.0.3**.

You will download a file called `gurobi11.0.3_linux64.tar.gz`. **Put it under
this directory.**

To use Gurobi with docker, we need a free "WLS Academic license". After login,
obtain a WLS license in the [user portal](https://portal.gurobi.com/iam/home),
and go to the [Web License
Manager](https://license.gurobi.com/manager/licenses) to download the license
on to your machine.

You will get a file called `gurobi.lic`. **Put it under this directory.**

So far, your directory should look like this (apart from the files in the directory map):

```
.
├── # Everything we have seen in the directory map
├── gurobi11.0.3_linux64.tar.gz
├── gurobi.lic
├── ModelSimSetup-20.1.0.711-linux.run
└── Xilinx_Vivado_SDK_2019.1_0524_1430.tar.gz
```


Congrats if you have reached this step! The steps above are the most
complicated part. The remaining steps are mostly automated.


## 3. Running the Experiments and Reproducing the Results

This section describes the steps for building the docker environment and
running the experiments.

### Build and Launch the Docker Image

We create a docker image called `crush-image` using the following command:
```sh
docker build -t crush-image .
```
Please be patient: this step will take some time (around 2 hours).

Now launch the image. Notice here we simply mount the license on the docker
image and the artifact folder in the home directory.
```sh
docker run -it -u dynamatic:dynamatic \
  -v $PWD:/home/dynamatic/asplos25-summer-crush \
  -v $PWD/gurobi.lic:/opt/gurobi1103/gurobi.lic:ro \
  crush-image /bin/bash
```

Let's see if the gurobi license is correctly configured (and please do this
before running the experiments):
```sh
gurobi_cl # This should show you some outputs saying everything looks fine.
```

### Building legacy Dynamatic

Assuming that you are in the docker image using the command above (you should
see the prompt like `dynamatic@b3afa353391b:/$`). Simply cd into the experiment
directory that we mounted:
```sh
cd ~/asplos25-summer-crush
bash build_project.sh
```
All the remaining steps assume that you are in `~/asplos25-summer-crush`.


### Running All Experiments and Generate Tables and Figures

This script runs all experiments in batch and generates the tables and figures.

```sh
bash run_all_experiments.sh
```

Yet, it is more recommended to run the experiments individually according to
the following steps as Vivado does not officially support Docker, and might
have (random) runtime errors; but the error usually disappears simply after
rerunning.

### Reproducing Table 2

These commands run HLS, simulation, and Vivado flow:
```sh
# Entries of "CRUSH"
python3 run_e1.py                          
# Entries of "In-order"
python3 run_e2.py                          
# Entries of "Naive"
python3 run_e3.py                          
# Draw the table in data/table.xlsx
python3 table_crush_vs_inorder_vs_naive.py 
```
To check the results, see `data/table.xlsx` (you may copy the table from the
container to your local machine).

Note: sometimes Vivado does not exit after placing & route; in this case,
manually end Vivado and rerun the experiment (you can comment out the list of
benchmarks that already finished in define.py, in the `BENCHMARKS` variable).

### Reproducing Table 3:

These commands run HLS, simulation, and Vivado flow:
```sh
# Entries of "Fast-token"
python3 run_e4.py              
# Entries of "CRUSH"
python3 run_e5.py              
# Entry of benchmark gsumif of "CRUSH"
bash manual_e4_fast_token_shared_gsumif.sh
# Entry of benchmark gsumif of "Fast-token"
bash manual_e5_fast_token_gsumif.sh
# Draw the table in data/table-fast.xlsx
python3 table_crush_vs_fast.py # see data/table-fast.xlsx report
```
To check the results, see `data/table-fast.xlsx` (you may copy the table from
the container to your local machine). We provide the HLS intermediate files of
benchmark "gsumif" before applying resource sharing since we need to manually
fix a minor functional bug of Dynamatic (hence the two bash scripts).

Note: sometimes Vivado does not exit after placing & route; in this case,
manually end Vivado and rerun the experiment (you can comment out the list of
benchmarks that already finished in define.py, in the `FAST_BENCHMARKS` variable).

### Generating Figure 7, 8, and 11

Figure 7, 8, and 11 are directly generated from the results in Table 2 and 3.

```sh
python3 plot_crush_vs_fast.py    # see crush_vs_fast_exec_time_ff_dsp.pdf
python3 plot_crush_vs_inorder.py # see crush_vs_inorder_exec_time_ff_dsp.pdf
python3 plot_crush_vs_naive.py   # see crush_vs_naive_exec_time_ff_dsp.pdf
```
To check the results, see `data/crush_vs_fast_exec_time_ff_dsp.pdf`,
`data/crush_vs_inorder_exec_time_ff_dsp.pdf`, and
`data/crush_vs_naive_exec_time_ff_dsp.pdf`.

### Generating Figure 9 and 10

```sh
# Generated characterization of individual sharing units
cd "sharing-overhead-comparison/char"
bash script.sh

# Plot figure 9 and 10
cd ..
python3 model.py
# see overhead_breakdown_crush.pdf and overhead_comparison_crush_vs_inorder.pdf
```

### Reproducing Table 1

```sh
bash run_e6.sh
```

Utilizations of circuit without sharing (No sharing in Table 1): 
```sh
less e6_big_benchmark/gesummv_float_unroll_naive/out/synth/utilization_post_syn.rpt
```
Please check the entries `Slice LUTs`, `Slice Registers`, `DSPs`.

Utilizations of circuit with sharing (CRUSH in Table 1): 
```sh
less e6_big_benchmark/gesummv_float_unroll_shared/out/synth/utilization_post_syn.rpt
```
Please check the entries `Slice LUTs`, `Slice Registers`, `DSPs`.

### Examine the results

The results are automatically parsed and formatted from various synthesis
reports and log files.  For instance, for the LUT, FF, Slice, critical path,
you can find them in the following locations:
```
e*_*/<BENCHMARK_NAME>/synth_rpts
├── timing_post_pr.rpt
├── utilization_post_pr_hierarchical.rpt
└── utilization_post_pr.rpt
```

The latency (cycles in Table 2 and 3) is obtained from simulation, the
simulation log file is in `e*_*/<BENCHMARK_NAME>/sim/HLS_VERIFY/transcript`. It
is calculated as the last `Time: XXXXX ns` in the transcript file divided by 4
ns (the clock period of the zero-delay simulation is 4 ns).

The optimization runtime is logged in
`e*_*/<BENCHMARK_NAME>/reports/resource_sharing.log`.
