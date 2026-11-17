#!/usr/bin/env python3
"""Smoke test: analyse examples/quick.cpp without library summaries and check the verdicts that do not
depend on the machine's libraries. The full suite is tests/run_tests.py (see the README for what it needs).

  FX_LLVM_BIN=<llvm 21>/bin python3 tests/smoke.py
"""
import json, os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPECT = {   # function: {class: (verdict, evidence or None)}
    'checksum(Order const&)': {'alloc': ('never', 'own'), 'lock': ('never', 'own'), 'syscall': ('never', 'own')},
    'make(long)': {'alloc': ('always', None)},
    'insert_locked(Order const&)': {'lock': ('always', None), 'alloc': ('may', None)},
    'route(Order const&, bool)': {'lock': ('may', None), 'alloc': ('may', None)},
}
with tempfile.TemporaryDirectory() as td:
    eff = os.path.join(td, 'effects.json')
    p = subprocess.run([sys.executable, os.path.join(ROOT, 'fn-effects'), os.path.join(ROOT, 'examples', 'quick.cpp'),
                        '--no-default-summaries', '--effects', eff, '-o', os.path.join(td, 'report.json'),
                        '--text', os.path.join(td, 'report.txt')], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit('fn-effects failed:\n' + p.stdout[-3000:] + p.stderr[-3000:])
    F = {v['demangled']: v for v in json.load(open(eff))['functions'].values()}
bad = 0
for fn, classes in EXPECT.items():
    for c, (verdict, evidence) in classes.items():
        s = F.get(fn, {}).get('signature', {}).get(c, {})
        ok = s.get('verdict') == verdict and (evidence is None or s.get('evidence') == evidence)
        print('PASS' if ok else 'FAIL', fn, c, 'expected', verdict, evidence or '', 'got', s.get('verdict'), s.get('evidence'))
        bad += not ok
sys.exit(1 if bad else 0)
