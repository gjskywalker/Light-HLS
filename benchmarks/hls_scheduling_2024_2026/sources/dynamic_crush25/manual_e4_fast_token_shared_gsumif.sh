#!/usr/bin/env bash

d_script=$PWD

# target directory
d_fast_shared_gsumif="./e4_fast_token_shared/gsumif"

f_sim_script=$d_script/flow_simulate.sh

f_syn_script=$d_script/flow_synthesize.sh

f_sharing_script=$d_script/flow_sharing.py

f_fast_buffer_bin=$d_script/legacy-dynamatic-fast-token/etc/dynamatic/Buffers/bin/buffers

# copy the manually fixed DOT:
rm -rf "$d_fast_shared_gsumif"
cp -r "$PWD/manually_generated_dots/gsumif" "$d_fast_shared_gsumif"

# Run HLS:
bash flow_compile_fast.sh \
  $(realpath ./legacy-dynamatic-fast-token) $d_fast_shared_gsumif gsumif.cpp

# Run optimization and measure the runtime:
start=$(date +%s.%N)
(
  filename="$(realpath $d_fast_shared_gsumif)/reports/gsumif"
  cd $d_fast_shared_gsumif
  $f_fast_buffer_bin buffers \
    -filename="$filename" \
    -period=6.0 \
    -solver=gurobi_cl \
    -timeout=120
)
end=$(date +%s.%N)
runtime=$(python3 -c "print($end - $start)")

# copy the manually fixed DOT:
rm -rf "$d_fast_shared_gsumif"
cp -r "$PWD/manually_generated_dots/gsumif" "$d_fast_shared_gsumif"

# runtime measured for fast shared (e4)
echo "total time: ${runtime}s" > \
  $d_fast_shared_gsumif/reports/resource_sharing.log

# sharing fast shared (e4)
python3 "$f_sharing_script" \
  $d_fast_shared_gsumif/reports/gsumif_optimized.dot \
  $d_fast_shared_gsumif/reports/gsumif_bbgraph.dot \
  $d_fast_shared_gsumif/reports/buffers.log \
  gsumif

# simulate fast shared (e4)
bash "$f_sim_script" \
  ./legacy-dynamatic \
  $d_fast_shared_gsumif/reports/gsumif_area_optimized.dot \
  gsumif

# synthesize fast shared (e4)
bash "$f_syn_script" \
  $d_fast_shared_gsumif


