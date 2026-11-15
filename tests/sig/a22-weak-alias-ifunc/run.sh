#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp -e 'call_*'
show call_
python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], 'alloc =', e['verdict'].get('alloc'), ' trusted:', e.get('trusted'), ' interposable:', e.get('interposable_definitions'))"
echo "--- as analysed (-O0 of whole.bc)"; build0 && check ./t0 call_ | grep -E "VIOLATION|RT.*alloc|GDB|PROGRAM|NOTRUN"
echo "--- ifunc resolver choosing the other body (QUIET=1 in the environment)"; QUIET=1 check ./t0 call_ifunc | grep -E "VIOLATION|RT.*alloc|GDB"
echo "--- the same bitcode linked with one more object that defines the weak symbols (override.c)"
$LLVM/clang -O1 -c override.c -o override.o && build0 override.o && mv t0 t0x && FX_SKIP_CLOSED=1 check ./t0x call_weak | grep -E "VIOLATION|RT.*alloc|GDB|PROGRAM|NOTRUN"
