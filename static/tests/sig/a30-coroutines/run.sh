#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
for O in 2 0; do
echo "=== fn-effects -O $O"
analyse a.cpp --std c++20 -O $O
python3 - <<'PY'
import json
E = json.load(open('effects.json'))['functions']
for f, v in sorted(E.items()):
    d = v.get('demangled', f)
    if any(k in d for k in ('lazy_alloc', 'eager_', 'counter', 'take_one', 'drive')):
        a = v['signature']['alloc']; print(f"{d[:60]:60s} alloc={a['verdict']}[{a.get('evidence')}]")
PY
build0 && check ./t0 lazy_alloc eager_ counter take_one drive | grep -E "VIOLATION|RT.*always|GDB|PROGRAM"
done
