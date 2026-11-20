# Verification and Resource Minimization Framework of Dataflow Circuits

## Get the framework

This framework depends on Dynamatic. Dynamatic is added as a git submodule.

```
$ git clone https://github.com/Jiahui17/dhls-formal.git
$ cd dhls-formal
$ git submodule init
$ git submodule update
```

## Install the framework

This framework builds on top of the [Dynamatic HLS compiler](https://github.com/lana555/dynamatic)

This framework depends on Python 3.8. You can get Python 3.8 using apt directly in Ubuntu (`sudo apt-get install python3.8*`).

For Debian and Arch, you can build it from the source `curl -O https://www.python.org/ftp/python/3.8.2/Python-3.8.2.tar.xz`

You might need sudo to run these scripts:

```sh
bash ./build_utils/1_install_deps.sh
bash ./build_utils/2_build_llvm60.sh
bash ./build_utils/3_setup_python38.sh
bash ./build_utils/4_build_dynamatic.sh
bash ./build_utils/5_install_modelsim.sh
bash ./build_utils/6_install_py_packages.sh
```

NuXmv is publicly available through `https://nuxmv.fbk.eu/`.

## Using the framework

### Activate the environments
I assume you are in the project root. First, activate the environments:

```sh
source ./.venv/bin/activate; source init_dhls.sh
```

### Synthesize the design using Dynamtic

Then go to our examples, for instance we want to try out on `fir` benchmark, cd
into the regression test directory:

```sh
cd ./etc/dynamatic/Regression_test/
```

Use your favorite text editor to put the benchmark (`fir`) that you want to run
in `filelist.lst` (e.g., by default, the file should contain all benchmarks
available in `./etc/dynamatic/Regression_test/examples`).

Then we can start the regression test by running the script:

```sh
cd ./etc/dynamatic/Regression_test/
bash regression_test.sh
```

### Using nuXmv to do verification and resource optimization

Again, let us consider the `fir` benchmark as an example:

```sh
cd ./etc/dynamatic/Regression_test/examples/fir
# generate formal model
dot2smv
dot2spec --backpressure
nuXmv -source ./reports/hw_smv/bdd.cmd

# simplify the netlist using proven properties
hdl2hdl.py --prop ./reports/property.rpt --hdl ./hdl/fir_optimized.vhd

# re-run the verifier for functional verification
run_verifier 
``` 

