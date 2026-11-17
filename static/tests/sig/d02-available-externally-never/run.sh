#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp -e 'f_*'
show _Z6f_find _Z7f_rfind _Z9f_compare _Z5f_ffo _Z6f_copy
python3 - <<'PY'
import json, subprocess
E = json.load(open('effects.json'))['functions']; R = json.load(open('report.json'))
ll = subprocess.run(['llvm-dis', 'w/whole.bc', '-o', '-'], capture_output=True, text=True).stdout
ae = {l.split('@')[1].split('(')[0] for l in ll.splitlines() if l.startswith('define available_externally')}
print('available_externally bodies in the module:', len(ae))
for f, v in E.items():
    if not v.get('demangled', '').startswith('f_'): continue
    seen, st = {f}, [f]
    while st:
        for c in E.get(st.pop(), {}).get('calls', []):
            if c['callee'] in E and c['callee'] not in seen: seen.add(c['callee']); st.append(c['callee'])
    through = sorted(seen & ae)
    nv = {c: s['evidence'] for c, s in v['signature'].items() if s['verdict'] == 'never' and c in ('alloc', 'free', 'lock', 'syscall')}
    print(v['demangled'], 'never:', nv, ' closure through available_externally:', [E[t]['demangled'][:70] for t in through])
for e in R['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc', 'free', 'lock', 'syscall')}, 'trusted:', e.get('trusted'))
PY
build0 && check ./t0 f_ | grep -E "VIOLATION|GDB"; echo "(run-time: no primitive observed unless a VIOLATION line is above)"
nm -D --defined-only $(ldd ./t0 | awk '/libstdc/{print $3}') | grep -c "4findEcm\|5rfindEPKcmm\|7compareEmmRKS4_\|17find_first_not_ofEPKcmm\|4copyEPcmm"
