#!/usr/bin/env python3
"""Run-time check of the per-function claims (README, "Per-function signatures", rule (g)).

  python3 tests/validate_always.py [CASE...]     (after tests/run_tests.py, which leaves tests/out/<case>/)

For each case, for the module-mode table (tests/out/<case>) and the component-mode table
(tests/out/<case>.pta-component), the module fn-effects analysed (work/whole.bc) is built twice:
  O0   clang++ -O0: the code that runs is the IR that was analysed;
  O2   clang++ -O2 -flto -fuse-ld=lld: what an optimising link makes of it.
Each build is run under gdb by tests/sig/check.py (the adversary's checker of round 6):
  always C                 every activation that returns by its own `ret` must have called a library primitive of C
                           in the same thread; `always (elidable)` is checked in the O0 build only;
  never C, plain evidence  (own, direct, indirect-pta, indirect-vtable, indirect-type) no activation may call one.
Classes: alloc, free, lock, syscall, throw. Writes tests/out/always-validation.txt; exit 1 on a counterexample.
"""
import json, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
CHECK = os.path.join(HERE, 'sig', 'check.py')
sys.path.insert(0, HERE)
from run_tests import llvm_bin
BUILDS = {'O0': ['-O0', '-g', '-no-pie', '-fno-pie'], 'O2': ['-O2', '-g', '-flto', '-fuse-ld=lld', '-no-pie']}


def run_one(name, sub, build, lb, lines, eff_override=None):
    """One table, one build. Returns (functions with a claim, functions run, always claims, never claims, violations) or None."""
    d = os.path.join(OUT, name + sub)
    eff, bc = eff_override or os.path.join(d, 'effects.json'), os.path.join(d, 'work', 'whole.bc')
    cj = os.path.join(HERE, 'cases', name, 'case.json')
    if not (os.path.exists(eff) and os.path.exists(bc) and os.path.exists(cj)):
        return None
    case = json.load(open(cj))
    if case.get('runtime_check'):
        if build == 'O0' and not sub:
            lines.append(f"{name}: not checked at run time ({case['runtime_check']})")
        return None
    exe = os.path.join(d, 'always.' + build)
    cmd = [os.path.join(lb, 'clang++')] + BUILDS[build] + [bc, '-o', exe, '-pthread', '-w'] + case.get('link_flags', '').split()
    p = subprocess.run(cmd, capture_output=True, text=True)
    tag = f"{name}{sub} [{build}]"
    if p.returncode != 0:
        lines.append(f"{tag}: not built ({(p.stderr.strip().splitlines() or ['?'])[-1][:120]})")
        return None
    env = dict(os.environ, FX_BUILD=build)
    try:
        g = subprocess.run([sys.executable, CHECK, exe, eff], capture_output=True, text=True, env=env, timeout=900)
    except subprocess.TimeoutExpired:
        lines.append(f"{tag}: timed out")
        return None
    m = re.search(r'CHECK-SUMMARY functions (\d+) run (\d+) always-claims (\d+) never-claims (\d+) violations (\d+)', g.stdout)
    if not m:
        lines.append(f"{tag}: checker did not finish: {g.stdout[-200:]} {g.stderr[-200:]}")
        return None
    r = tuple(int(x) for x in m.groups())
    for l in g.stdout.splitlines():
        if l.startswith(('VIOLATION', 'GDB-PROBLEM')):
            lines.append(f"{tag}: {l}")
    lines.append(f"{tag}: functions with a claim {r[0]}, run {r[1]}, always claims {r[2]}, never claims {r[3]}, violations {r[4]}")
    return r


def main(argv=None, quiet=False, inject=None):
    lb = llvm_bin()
    names = (sys.argv[1:] if argv is None else argv) or sorted(x for x in os.listdir(os.path.join(HERE, 'cases')) if os.path.isdir(os.path.join(HERE, 'cases', x)))
    lines, tot = [], {}
    for n in names:
        for sub in ('', '.pta-component'):
            for build in BUILDS:
                eff = None
                if inject:  # negative control: false claims the checker must refute
                    src = os.path.join(OUT, n + sub, 'effects.json')
                    if not os.path.exists(src):
                        continue
                    e = json.load(open(src))
                    for f, c, v in inject:
                        e['functions'][f]['signature'][c] = {'verdict': v, 'evidence': 'own'}
                    eff = os.path.join(OUT, n + sub, 'effects.injected.json')
                    json.dump(e, open(eff, 'w'))
                r = run_one(n, sub, build, lb, lines, eff)
                if r:
                    t = tot.setdefault((sub or '.module', build), [0, 0, 0, 0, 0])
                    for i in range(5):
                        t[i] += r[i]
    for (sub, build), t in sorted(tot.items()):
        lines.append(f"TOTAL {sub[1:]} {build}: functions with a claim {t[0]}, run and checked {t[1]}, always claims {t[2]}, never claims {t[3]}, counterexamples {t[4]}")
    if not inject:
        open(os.path.join(OUT, 'always-validation.txt'), 'w').write('\n'.join(lines) + '\n')
    if quiet:
        return tot, lines
    print('\n'.join(l for l in lines if 'VIOLATION' in l or l.startswith('TOTAL') or 'not built' in l or 'did not' in l))
    sys.exit(1 if any(t[4] for t in tot.values()) else 0)


if __name__ == '__main__':
    main()
