#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
$CXX -O2 -g -emit-llvm -c lib.cpp -o lib.bc     # plain bitcode: no !type metadata on its vtables
for mode in module component; do
  echo "--- fn-effects app.cpp lib.bc --pta-scope $mode -e dispatch -e dispatch_tagged -e pull -e pull_tagged"
  analyse "app.cpp lib.bc" --pta-scope $mode -e dispatch -e dispatch_tagged -e pull -e pull_tagged
  show dispatch pull
  python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['entry'], 'alloc =', e['verdict'].get('alloc'), ' trusted:', e.get('trusted'))
for ic in r.get('indirect_calls', []): print('INDIRECT', ic['site']['func'], ic['rule'], ic['targets'])"
  build0 && check ./t0 dispatch pull | grep -E "VIOLATION|RT.*always|GDB|PROGRAM|NOTRUN"
  build2 && check ./t2 dispatch pull | grep -E "VIOLATION|RT.*always|GDB|PROGRAM|NOTRUN|NOSYM"
done
