#!/bin/bash

LEGACY_DYNAMATIC_DIR=$1
d_comp=$2
f_top=$3

mkdir -p $d_comp

cd $d_comp

source $LEGACY_DYNAMATIC_DIR/init_dhls.sh

compile "${f_top}" . "-use-lsq=false" > /dev/null
