#!/usr/bin/env bash

# Kill the whole script on Ctrl+C
trap "exit" INT

# actual path of the dynamatic directory
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

source $SCRIPT_DIR/scripts/environment.sh

#===========================================================================#


BENCHMARKS="fir iir matvec if_loop_add gsum gsumif kernel_2mm kernel_3mm"

echo "You currently selected the following options:"
echo "Benchmarks: $BENCHMARKS"
read -p "Continue? (Y/N): " confirm && [[ $confirm == [yY] || $confirm == [yY][eE][sS] ]] || exit 1

python3 $SCRIPT_DIR/e2_static_hls.py \
	-jobs run_experiments run_vivado_synth \
	-benchmarks if_loop_add gsum gsumif \
	-techniques "static_no_share"

python3 $SCRIPT_DIR/e2_static_hls.py \
	-jobs run_experiments run_vivado_synth \
	-benchmarks fir iir matvec if_loop_add gsum gsumif kernel_2mm kernel_3mm \
	-techniques "static"

echo "experiment done!"

python3 scripts/p1_vs_static_disable_share.py
