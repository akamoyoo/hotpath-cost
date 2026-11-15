#!/bin/bash
# Differential: the tool's own test outputs (module solve vs --pta-scope component), every function and class. Read-only on ir/tests/out.
cd "$(dirname "$0")"; . ../common.sh
for d in $HP/tests/out/*.pta-component; do
  b=${d%.pta-component}
  [ -f $b/effects.json ] && [ -f $d/effects.json ] && python3 $SIG/diffmode.py $b/effects.json $d/effects.json $(basename $b)
done > diff.txt 2>&1
grep "^DIFF" diff.txt; grep -c "^diffmode" diff.txt | sed "s/^/cases compared: /"
# run-time check of the `always` claims that exist only in component mode (the tool's validate_always.py runs the module-mode tables only)
for c in attack_20_pta_held_batch virtual_two_impls; do
  d=$HP/tests/out/$c.pta-component
  [ -f $d/work/whole.bc ] || d=$HP/tests/out/$c
  echo "--- $c: -O0 of $(basename $d)/work/whole.bc against the component-mode table"
  $TS $CXX -O0 -g -no-pie -fno-pie $d/work/whole.bc -o t0.$c -pthread -w && python3 $SIG/check.py ./t0.$c $HP/tests/out/$c.pta-component/effects.json e_placement e_vbase main 2>&1 | grep -E "VIOLATION|RT.*always|GDB|NOTRUN"
done
