#!/usr/bin/env python3
"""check.py EXE effects.json [FUNC-substring...]
Run EXE under gdb. For every function of the effect table (optionally only those matching) that has a symbol in EXE:
  always C            -> every activation that returns normally must have hit a primitive of C (same thread)
  never C [own|direct]-> no activation may hit a primitive of C (FX_NEVER_ALL=1: every `never`, whatever its evidence)
Primitive = the policy's `primitives` patterns, on every dynamic symbol of every library EXE loads and on EXE's own symbols
(README rule (b): a primitive is a library function, so by default only library symbols are breakpoints; FX_LIBONLY=0 also
counts the program's own functions of those names). FX_BUILD=O2: `always (elidable)` claims are not checked.
FX_SKIP_CLOSED=1: closed-world `always` claims are not checked. Adopted from the adversary's attack-sig/check.py (round 6)."""
import json, os, re, subprocess, sys
HP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LB = os.environ['FX_LLVM_BIN']
CLASSES = ['alloc', 'free', 'lock', 'syscall', 'throw']
exe, eff, pats = sys.argv[1], sys.argv[2], sys.argv[3:]
host = exe
solib = os.environ.get('FX_SOLIB')   # the analysed module is this shared object, loaded by EXE with dlopen
if solib: exe = solib
pol = json.load(open(HP + '/policies/hotpath.json'))
P = {c: [p[3:] if p.startswith('re:') else '^' + re.escape(p) + '$' for p in pol['classes'][c].get('primitives', [])] for c in CLASSES}
F = json.load(open(eff))['functions']
nm = subprocess.run([LB + '/llvm-nm', '--defined-only', exe], capture_output=True, text=True).stdout
addrs = {}
anchor = None
for l in nm.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW': addrs[p[2]] = '0x' + p[0]
    if len(p) == 3 and p[1] == 'T' and solib and not anchor and not p[2].startswith('_'): anchor = p[2]
if solib: os.environ['FX_ANCHOR'] = anchor
claims = {}
allnever = os.environ.get('FX_NEVER_ALL') == '1'
# the evidence levels of a plain `none` (README): the claims that are guarantees
NEVER_EV = os.environ.get('FX_NEVER_EV', 'own,direct,indirect-pta,indirect-vtable,indirect-type').split(',')
for f, v in F.items():
    if f == 'main' or f not in addrs: continue
    if pats and not any(p in f for p in pats): continue
    d = {}
    for c in CLASSES:
        s = v.get('signature', {}).get(c, {})
        if s.get('verdict') == 'always':
            rests = s.get('always', {}).get('rests_on', [])
            # a closed-world claim is not a guarantee: it is not checked in a build that breaks the assumption on purpose
            if os.environ.get('FX_SKIP_CLOSED') == '1' and (s.get('evidence') == 'closed-world' or 'closed-world' in rests): continue
            # always (elidable): true of the analysed IR (-O0 build); the optimiser may remove the call afterwards
            if os.environ.get('FX_BUILD') == 'O2' and s.get('qualifier') == 'elidable': continue
            d[c] = 'always'
        elif s.get('verdict') == 'never' and (allnever or s.get('evidence') in NEVER_EV): d[c] = 'never:' + str(s.get('evidence'))
    if d: claims[f] = d
names = {c: set() for c in P}
env0 = dict(os.environ)
ld = subprocess.run(['ldd', host], capture_output=True, text=True, env=env0).stdout
for lib in [m.group(1) for m in re.finditer(r'=> (/\S+)', ld)] + [m.group(1) for m in re.finditer(r'^\s*(/\S+ld-linux\S+)', ld, re.M)]:
    for l in subprocess.run(['nm', '-D', '--defined-only', lib], capture_output=True, text=True).stdout.split('\n'):
        p = l.split()
        if len(p) < 3 or p[1] not in 'TtWwi': continue
        sym = p[2].split('@')[0]
        for c, rxs in P.items():
            if any(re.search(rx, sym) for rx in rxs): names[c].add(sym)
own_prims = sorted(sym for c in P for sym in addrs if any(re.search(rx, sym) for rx in P[c]))
if os.environ.get('FX_LIBONLY', '1') == '1':   # rule (b): a primitive is a library function   # count only primitives that live in a library: the program's own functions of that name are not breakpoints
    libaddr = {}
    print('LIBONLY: program-defined functions with a primitive name are not counted:', ' '.join(own_prims))
    names = {c: {('*LIB*' + s) for s in v if s not in addrs} for c, v in names.items()}
else:
    for c in P:
        for sym in addrs:
            if any(re.search(rx, sym) for rx in P[c]): names[c].discard(sym); names[c].add('*' + addrs[sym])  # by address: a name would also stop at inlined copies
dis = subprocess.run([LB + '/llvm-objdump', '-d', '--no-show-raw-insn', exe], capture_output=True, text=True).stdout
rets = [m.group(1) for m in re.finditer(r'^\s*([0-9a-f]+):\s+ret[ql]?\s*$', dis, re.M)]
os.environ['FX_RETS'] = json.dumps(rets)
env = dict(os.environ, FX_CLAIMS=json.dumps(claims), FX_PRIMS=json.dumps({c: sorted(v) for c, v in names.items()}), FX_ADDRS=json.dumps(addrs))
args = os.environ.get('FX_ARGS', '').split()
g = subprocess.run(['gdb', '-q', '-batch', '-x', HP + '/tests/sig/sig_gdb.py', '--args', host] + args, capture_output=True, text=True, env=env, timeout=900)
bad = 0
for l in g.stdout.splitlines():
    if l.startswith(('RT ', 'VIOLATION', 'NOTRUN', '#', 'PROGRAM', 'LIBONLY', 'DBG')): print(l)
    if l.startswith('VIOLATION'): bad += 1
if g.returncode != 0 or 'RT-END' not in g.stdout:
    print('GDB-PROBLEM', g.stdout[-600:], g.stderr[-600:])
missing = [f for f in F if pats and any(p in f for p in pats) and f not in addrs and f != 'main']
for f in missing: print('NOSYMBOL (inlined or removed in this build)', f)
nal = sum(1 for d in claims.values() for k in d.values() if k == 'always'); nne = sum(1 for d in claims.values() for k in d.values() if k != 'always')
ran = {l.split()[1] for l in g.stdout.splitlines() if l.startswith('RT ')}
print(f"CHECK-SUMMARY functions {len(claims)} run {len(ran)} always-claims {nal} never-claims {nne} violations {bad}")
