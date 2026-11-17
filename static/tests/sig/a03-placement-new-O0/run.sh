#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
for O in 0 2; do
  echo "=== fn-effects -O $O"; analyse a.cpp -O $O -e place -e place_arr; show _Z5placel _Z9place_arrl nothrow_new
  python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], 'alloc =', e['verdict'].get('alloc'))"
  build0 && check ./t0 place nothrow | grep -E "VIOLATION|RT.*alloc|GDB"
  FX_LIBONLY=1 check ./t0 place nothrow | grep -E "VIOLATION|LIBONLY|GDB"
done
