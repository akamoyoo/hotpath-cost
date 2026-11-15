#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp
python3 - <<'PY'
import json
E = json.load(open('effects.json'))['functions']
want = {'shrink': ['alloc', 'free'], 'grow': ['alloc', 'free'], 'wait_ready': ['lock', 'syscall'], 'block_sem': ['lock', 'syscall'], 'unmap': ['free', 'syscall'],
        'swallow': ['free', 'throw'], 'closef': ['free', 'syscall', 'lock'], 'bye': ['syscall'], 'cxx_wait': ['lock', 'syscall'], 'join': ['free', 'lock', 'syscall']}
for f, v in E.items():
    d = v.get('demangled', f).split('(')[0]
    if d in want:
        print(f"{d:12s}", "  ".join(f"{c}: {v['signature'][c]['verdict']}, {v['signature'][c].get('evidence')}, account {v['signature'][c].get('account')}" + (f" of {v['signature'][c].get('consequence_of')}" if v['signature'][c].get('consequence_of') else '') for c in want[d]))
PY
