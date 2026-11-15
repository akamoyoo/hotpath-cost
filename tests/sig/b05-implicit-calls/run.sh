#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp --std c++20
python3 - <<'PY'
import json
E = json.load(open('effects.json'))['functions']
for f, v in sorted(E.items(), key=lambda kv: kv[1].get('demangled', '')):
    d = v.get('demangled', f)
    if f.startswith('_Z') and not d.startswith(('std::', 'Loud', 'Guard', 'Conv', 'Cmp', 'SB', 'It', 'Range', 'P', 'Member', 'Holder', '__', 'operator', 'TLS', 'thread-local', 'void std', 'bool std', 'by_value', 'with_default', 'defarg', 'long SB', 'guard', 'main')):
        print(f"{d[:42]:42s}", " ".join(f"{c}={v['signature'][c]['verdict']}[{v['signature'][c].get('evidence')}]" for c in ('alloc', 'free', 'lock', 'throw')))
PY
build0 -latomic && check ./t0 | grep -E "VIOLATION|GDB|PROGRAM"; echo "(end -O0)"
build2 -latomic && check ./t2 | grep -E "VIOLATION|GDB|PROGRAM"; echo "(end -O2)"
