#!/usr/bin/env bash

d_script=$PWD

# target directory
d_fast_normal_gsumif="./e5_fast_token/gsumif"

f_sim_script=$d_script/flow_simulate.sh

f_syn_script=$d_script/flow_synthesize.sh

f_sharing_script=$d_script/flow_sharing.py

f_fast_buffer_bin=$d_script/legacy-dynamatic-fast-token/etc/dynamatic/Buffers/bin/buffers

# copy the manually fixed DOT:
rm -rf "$d_fast_normal_gsumif"
cp -r "$PWD/manually_generated_dots/gsumif" "$d_fast_normal_gsumif"

# Run HLS:
bash flow_compile_fast.sh \
  $(realpath ./legacy-dynamatic-fast-token) $d_fast_normal_gsumif gsumif.cpp

# Run optimization and measure the runtime:
start=$(date +%s.%N)
(
  filename="$(realpath $d_fast_normal_gsumif)/reports/gsumif"
  cd $d_fast_normal_gsumif
  $f_fast_buffer_bin buffers \
    -filename="$filename" \
    -period=6.0 \
    -solver=gurobi_cl \
    -timeout=120
)
end=$(date +%s.%N)
runtime=$(python3 -c "print($end - $start)")

# copy the manually fixed DOT:
rm -rf "$d_fast_normal_gsumif"
cp -r "$PWD/manually_generated_dots/gsumif" "$d_fast_normal_gsumif"

cp $d_fast_normal_gsumif/reports/gsumif_optimized.dot \
  $d_fast_normal_gsumif/reports/gsumif_area_optimized.dot

# use the measured runtime for fast normal (e5)
echo "total time: ${runtime}s" > \
  $d_fast_normal_gsumif/reports/resource_sharing.log

# simulate fast normal (e5):
bash $f_sim_script \
  ./legacy-dynamatic \
  $d_fast_normal_gsumif/reports/gsumif_area_optimized.dot \
  gsumif

# synthesize normal (e5):
bash $f_syn_script \
  $d_fast_normal_gsumif
