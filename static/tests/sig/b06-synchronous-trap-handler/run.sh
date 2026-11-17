#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp -e checkpoint -e notify
show checkpoint notify
python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc', 'lock', 'abort')}, ' trusted:', e.get('trusted'))"
build0
echo "--- ./t0 (-O0 of whole.bc), no debugger:"; ./t0; echo "exit=$?"
build2 && echo "--- ./t2 (-O2 -flto):" && ./t2; echo "exit=$?"
