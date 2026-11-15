#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse "a.cpp lib.cpp"
python3 - <<'PY'
import json
E = json.load(open('effects.json'))['functions']
for f, v in sorted(E.items()):
    if v.get('demangled', '').startswith('e_'):
        print(f"{v['demangled']:24s}", " ".join(f"{c}={v['signature'][c]['verdict']}{'(elidable)' if v['signature'][c].get('qualifier') == 'elidable' else ''}" for c in ('alloc', 'free', 'lock', 'syscall', 'throw') if v['signature'][c]['verdict'] == 'always'))
PY
echo "--- -O0 of whole.bc (the tool's own check)"; build0 && check ./t0 _Z | grep -E "VIOLATION|GDB|PROGRAM" ; echo "(end -O0)"
echo "--- -O2 -flto link of whole.bc"; build2 && check2 ./t2 _Z | grep -E "VIOLATION|GDB|PROGRAM|NOSYM"; echo "(end -O2)"
