#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp --std c++23
python3 - <<'PY'
import json
E = json.load(open('effects.json'))['functions']
for f, v in E.items():
    if 'St' in f or '_ZN' in f and 'Ctor' not in f: continue
    print(f"{v.get('demangled', f):40s} alloc={v['signature']['alloc']['verdict']}[{v['signature']['alloc'].get('evidence')}]")
PY
build0 && check ./t0 | grep -E "VIOLATION|always|GDB|PROGRAM|NOTRUN"
