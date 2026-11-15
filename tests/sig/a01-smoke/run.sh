#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp
show _Z4purei _Z2ali _Z2lkv thr_catch _Z2clP
build0 && check ./t0
