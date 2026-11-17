#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp
show _Z4fireP _Z9fire_baseP _Z6fire_aP _Z8fire_midP
python3 $HP/fn-effects --from-effects effects.json --why fire_a --class alloc | head -8
python3 $HP/fn-effects --from-effects effects.json --why fire_mid --class alloc | head -8
build0 && check ./t0 fire | grep -E "VIOLATION|RT.*always|GDB|PROGRAM|NOTRUN"
echo "--- component mode"; analyse a.cpp --pta-scope component; show _Z4fireP _Z9fire_baseP _Z6fire_aP _Z8fire_midP
build0 && check ./t0 fire | grep -E "VIOLATION|RT.*always|GDB|PROGRAM|NOTRUN"
