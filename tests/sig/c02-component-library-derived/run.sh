#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
$CXX -O2 -c plugin.cpp -o plugin.o && $CXX -O2 -c plugin2.cpp -o plugin2.o     # the library: machine code only
for mode in module component; do
  echo "=== fn-effects app.cpp --pta-scope $mode -e pull -e get    (plugin.o, plugin2.o are a library without bitcode)"
  analyse app.cpp --pta-scope $mode -e pull -e get -e pull_loud
  show _Z4pullP _Z3getP _Z9pull_loud
  python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc', 'indirect-unknown')}, ' trusted:', e.get('trusted'))
for ic in r.get('indirect_calls', []): print('INDIRECT', ic['site']['func'], ic['rule'], ic['targets'])"
  build0 plugin.o plugin2.o && check ./t0 _Z4pullP _Z3getP _Z9pull_loud | grep -E "VIOLATION|RT.*always|GDB"
done
for prec in rta type; do
  echo "=== for comparison: --precision $prec (module)"; analyse app.cpp --precision $prec -e get; show _Z3getP
  python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc', 'indirect-unknown')}, ' trusted:', e.get('trusted'))"
done
