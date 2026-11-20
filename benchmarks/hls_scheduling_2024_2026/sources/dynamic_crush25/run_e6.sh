#!/usr/bin/env bash

# This script reproduces data in Table 1

# Dynamatic flags
BUFFER_ALGORITHM="--buffer-algorithm on-merges"
HDL="vhdl"

# Directories
d_script=$PWD

dynamatic_path="$HOME/dynamatic"

# Target directory
d_benchmark_src="./benchmarks-mlir/gesummv_float_unroll"

d_benchmark_tgt="$d_script/e6_big_benchmark"

d_benchmark_naive="$d_benchmark_tgt/gesummv_float_unroll_naive"
d_benchmark_shared="$d_benchmark_tgt/gesummv_float_unroll_shared"

mkdir -p "$d_benchmark_naive"
mkdir -p "$d_benchmark_shared"

cp $d_benchmark_src/gesummv_float_unroll.c $d_benchmark_src/gesummv_float_unroll.h $d_benchmark_naive
cp $d_benchmark_src/gesummv_float_unroll.c $d_benchmark_src/gesummv_float_unroll.h $d_benchmark_shared

# Run Dynamatic (MLIR)
SHARING=""
s_source_file="$d_benchmark_naive/gesummv_float_unroll.c"
[ -f "$s_source_file" ] || \
  { echo "Source file $s_source_file does not exist!"; exit 1; }
echo "set-dynamatic-path $dynamatic_path; \
  set-src $s_source_file; \
  set-clock-period 8; \
  compile $SHARING $BUFFER_ALGORITHM; \
  write-hdl --hdl $HDL; \
  exit" \
  | $dynamatic_path/bin/dynamatic --exit-on-failure


SHARING="--sharing"
s_source_file="$d_benchmark_shared/gesummv_float_unroll.c"
[ -f "$s_source_file" ] || \
  { echo "Source file $s_source_file does not exist!"; exit 1; }
echo "set-dynamatic-path $dynamatic_path; \
  set-src $s_source_file; \
  set-clock-period 8; \
  compile $SHARING $BUFFER_ALGORITHM; \
  write-hdl --hdl $HDL; \
  exit" \
  | $dynamatic_path/bin/dynamatic --exit-on-failure

mkdir -p $d_benchmark_naive/out/synth/hdl
mkdir -p $d_benchmark_shared/out/synth/hdl

cp $d_benchmark_naive/out/hdl/*.vhd $d_benchmark_naive/out/synth/hdl
cp $d_benchmark_shared/out/hdl/*.vhd $d_benchmark_shared/out/synth/hdl

cp $d_script/vivado_fpu_ip_wrappers/hdl/*.vhd $d_benchmark_naive/out/synth/hdl
cp $d_script/vivado_fpu_ip_wrappers/hdl/*.vhd $d_benchmark_shared/out/synth/hdl
cp $d_script/vivado_fpu_ip_wrappers/hdl/*.tcl $d_benchmark_naive/out/synth/hdl
cp $d_script/vivado_fpu_ip_wrappers/hdl/*.tcl $d_benchmark_shared/out/synth/hdl

cp $d_script/vivado_fpu_ip_wrappers/*.tcl $d_benchmark_naive/out/synth
cp $d_script/vivado_fpu_ip_wrappers/*.tcl $d_benchmark_shared/out/synth
cp $d_script/vivado_fpu_ip_wrappers/*.xdc $d_benchmark_naive/out/synth
cp $d_script/vivado_fpu_ip_wrappers/*.xdc $d_benchmark_shared/out/synth


cd $d_benchmark_naive/out/synth
vivado -mode batch -source synthesize.tcl
cd $d_benchmark_shared/out/synth
vivado -mode batch -source synthesize.tcl


