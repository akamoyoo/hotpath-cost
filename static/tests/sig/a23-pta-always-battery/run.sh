#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
for mode in module component; do
echo "=== --pta-scope $mode"
analyse a.cpp --pta-scope $mode
python3 - <<'PY'
import json
E = json.load(open('effects.json'))['functions']; R = json.load(open('report.json'))
rule = {}
for ic in R.get('indirect_calls', []): rule.setdefault(ic['site']['func'], []).append(f"{ic['rule']}:{ic['targets']}")
for f, v in sorted(E.items()):
    if 'via_' in f or 'va_call' in f:
        a = v['signature']['alloc']
        print(f"{v.get('demangled', f):28s} alloc={a['verdict']:7s}[{a.get('evidence')}] rests_on={'+'.join(a.get('always', {}).get('rests_on', []))}  calls: {rule.get(f)}")
PY
build0 && check ./t0 via_ va_call | grep -E "VIOLATION|RT.*always|GDB|PROGRAM|NOTRUN"
done
