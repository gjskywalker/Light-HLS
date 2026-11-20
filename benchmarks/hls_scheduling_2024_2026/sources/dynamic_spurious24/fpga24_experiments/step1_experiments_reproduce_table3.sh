#!/usr/bin/env bash

# Kill the whole script on Ctrl+C
trap "exit" INT

# actual path of the dynamatic directory
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

source $SCRIPT_DIR/scripts/environment.sh

#===========================================================================#

BENCHMARKS="fir iir matvec if_loop_add gsum gsumif kernel_2mm kernel_3mm"
TECHNIQUES="no_opt occupancy_match latency_match latency_and_occupancy_match"
JOBS="copy_files run_experiments run_vivado_synth parse_reports"

echo "You currently selected the following options:"
echo "Benchmarks: $BENCHMARKS"; echo "Techniques: $TECHNIQUES"; echo "Jobs: $JOBS"
read -p "Continue? (Y/N): " confirm && [[ $confirm == [yY] || $confirm == [yY][eE][sS] ]] || exit 1

python3 $SCRIPT_DIR/e1_technique_comparison.py \
	-jobs $JOBS \
	-benchmarks $BENCHMARKS \
	-techniques $TECHNIQUES

exit 0
