#!/bin/bash
d_project=$1

source /tools/Xilinx/Vivado/2019.1/settings64.sh

synthesis_script=$(realpath ./run_vivado2019.py)

cd $d_project

python3 $synthesis_script -clock=6
