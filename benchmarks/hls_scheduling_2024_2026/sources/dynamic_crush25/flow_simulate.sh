#!/usr/bin/env bash

# from DOT to simulation

d_legacy_dynamatic=$1

f_dot=$2

func_name=$3

dot2vhdl_bin=$d_legacy_dynamatic/etc/*/dot2vhdl/bin/dot2vhdl

source $d_legacy_dynamatic/init_dhls.sh

d_root=$(realpath $(dirname -- $f_dot)/..)

write_hdl $d_root ${f_dot/.dot/}

echo $d_root

d_report=$d_root/reports
d_hdl=$d_root/hdl
d_sim=$d_root/sim

mv ${f_dot/.dot/.vhd} $d_hdl

cp $d_legacy_dynamatic/etc/*/components/*{.vhd,.v,.tcl} $d_hdl


# perform functional verification
mkdir -p "$d_sim"/{C_SRC,VHDL_SRC,REF_OUT,HLS_VERIFY,INPUT_VECTORS,VHDL_OUT}

# root directory of verify project
VERIFY_DIR="$d_sim"

# copy the c++ source code into directory C_SRC
cp "${d_root}"/src/*.{cpp,h} "${VERIFY_DIR}"/C_SRC 2> /dev/null

# copy the hdl source code into directory VHDL_SRC
cp "${d_root}"/hdl/* "${VERIFY_DIR}"/VHDL_SRC 2> /dev/null

rm -r "${d_root}/sim/HLS_VERIFY/work" 2> /dev/null

# verifier requires you to be in HLS_VERIFY directory to work
cd "${d_root}/sim/HLS_VERIFY"

echo "starting simulation!"

# run the verifier and save the log
hlsverifier \
	cover -aw32 "../C_SRC/${func_name}.cpp" \
	"../C_SRC/${func_name}.cpp" "${func_name}" 2>&1 \
	| tee ${d_root}/reports/hls_verify.log \
	|| fail 'hlsverifier failed!'
