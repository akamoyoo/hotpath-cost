#!/bin/bash
# The run-time checker itself (check.py / sig_gdb.py, adopted from the adversary in round 6).
# (1) A false `always alloc` must be refuted: lazy_alloc's ramp only calls Task::promise_type::operator new (a class-specific
#     operator, a static pool). A breakpoint on the unqualified name _Znwm would also stop there and pass the claim.
# (2) A function that leaves by longjmp / setcontext has not returned normally: no counterexample may be reported for it.
cd "$(dirname "$0")"; . ../common.sh
(cd ../a30-coroutines && analyse a.cpp --std c++20 >/dev/null; build0)
echo "--- (1) injected false claim: lazy_alloc(long) always alloc"
(cd ../a30-coroutines && python3 - <<'PY'
import json, subprocess
e = json.load(open('effects.json')); e['functions']['_Z10lazy_allocl']['signature']['alloc'] = {'verdict': 'always', 'evidence': 'injected'}
json.dump(e, open('effects.injected.json', 'w'))
out = subprocess.run(['python3', '../check.py', './t0', 'effects.injected.json', '_Z10lazy_allocl'], capture_output=True, text=True).stdout
print('REFUTED-INJECTED' if 'VIOLATION always alloc: _Z10lazy_allocl' in out else 'INJECTED-CLAIM-PASSED (checker too weak)')
PY
)
(cd ../a10-cfg-normal-return && analyse a.cpp --std c++23 >/dev/null && build0)
echo "--- (2) injected claims jumper(long), lj(long), ctx_jumper() always lock: they leave by __builtin_longjmp / longjmp / setcontext and never return, so nothing can be refuted"
(cd ../a10-cfg-normal-return && python3 - <<'PY'
import json, subprocess
e = json.load(open('effects.json'))
fs = ['_Z6jumperl', '_Z2ljl', '_Z10ctx_jumperv']
for f in fs: e['functions'][f]['signature']['lock'] = {'verdict': 'always', 'evidence': 'injected'}
json.dump(e, open('effects.injected.json', 'w'))
out = subprocess.run(['python3', '../check.py', './t0', 'effects.injected.json'] + fs, capture_output=True, text=True).stdout
bad = [l for l in out.splitlines() if l.startswith('VIOLATION always lock')]
print('NO-FALSE-COUNTEREXAMPLE' if not bad else 'FALSE-COUNTEREXAMPLE ' + str(bad))
print('\n'.join(l for l in out.splitlines() if l.startswith('RT') and ' lock' in l))
PY
)
