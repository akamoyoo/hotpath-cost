#!/usr/bin/env python3
"""Run every test case of fn-effects: static expectations, --diff checks, and the RealtimeSanitizer comparison.

  python3 tests/run_tests.py [--no-rtsan] [CASE...]

Each directory under tests/cases has a case.json: sources, entries, optional cflags, policy, summaries,
variants (other cflags with their own expectations), a diff check, and the expectations, written before
the first run. Output goes to tests/out/. Exit status 0 only if every check passed and RealtimeSanitizer
reported nothing the static report does not cover.
"""
import argparse, json, os, re, shlex, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
IR = os.path.dirname(HERE)
HPIR = os.path.join(IR, 'fn-effects')
OUT = os.path.join(HERE, 'out')


def llvm_bin():
    b = os.environ.get('FX_LLVM_BIN')
    if b:
        return b
    import shutil
    w = shutil.which('llvm-config')
    if w:
        return subprocess.run([w, '--bindir'], capture_output=True, text=True).stdout.strip()
    # fall back to the LLVM the plugin was built against
    cache = os.path.join(IR, 'build', 'CMakeCache.txt')
    if os.path.exists(cache):
        for line in open(cache):
            if line.startswith('LLVM_DIR:'):
                d = line.split('=', 1)[1].strip()
                b = os.path.normpath(os.path.join(d, '..', '..', '..', 'bin'))
                if os.path.exists(os.path.join(b, 'opt')):
                    return b
    sys.exit('set FX_LLVM_BIN to the bin directory of an LLVM 21 installation')


class Result:
    def __init__(self):
        self.lines, self.fail = [], 0

    def ok(self, cond, msg):
        self.lines.append(('PASS ' if cond else 'FAIL ') + msg)
        if not cond:
            self.fail += 1
        return cond

    def note(self, msg):
        self.lines.append('     ' + msg)


def find_entry(rep, name):
    for e in rep['entries']:
        if name in (e['demangled'], e['entry']):
            return e
    for e in rep['entries']:
        if e['demangled'].startswith(name):
            return e
    return None


def check_expect(rep, exp, R, tag):
    e = find_entry(rep, exp['entry'])
    if not R.ok(e is not None, f"{tag}: entry {exp['entry']} present"):
        return
    for c, v in exp.get('verdict', {}).items():
        R.ok(e['verdict'].get(c) == v, f"{tag}: {exp['entry'][:40]} verdict {c} = {v} (got {e['verdict'].get(c)})")
    for h in exp.get('has', []):
        def m(f):
            if f['class'] != h['class']:
                return False
            if 'symbol' in h and f['symbol'] != h['symbol']:
                return False
            if 'symbol_re' in h and not re.search(h['symbol_re'], f['symbol']):
                return False
            if 'subclass' in h and f.get('subclass') != h['subclass']:
                return False
            if 'temp' in h and f['temp'] != h['temp']:
                return False
            if 'chain_contains' in h and h['chain_contains'] not in f['chain']:
                return False
            if 'edge' in h and h['edge'] not in f.get('edges', []) and f.get('via') != h['edge']:
                return False
            return True
        R.ok(any(m(f) for f in e['findings']), f"{tag}: {exp['entry'][:40]} has finding {h}")
    if 'all_temp' in exp:
        bad = [f for f in e['findings'] if f['temp'] != exp['all_temp'] and f['class'] not in ('atomic', 'unwind', 'ub-unreachable')]
        R.ok(not bad, f"{tag}: {exp['entry'][:40]} every hazard finding is {exp['all_temp']}" +
             (f" (not: {[(f['class'], f['symbol'], f['temp']) for f in bad][:4]})" if bad else ''))
    for c in exp.get('none_hot', []):
        bad = [f for f in e['findings'] if f['class'] == c and f['temp'] == 'likely']
        R.ok(not bad, f"{tag}: {exp['entry'][:40]} no hot {c}")
    if 'assumption' in exp:
        a = exp['assumption']
        def ma(x):
            if x.get('kind') != a['kind']:
                return False
            if 'rule' in a and x.get('rule') != a['rule']:
                return False
            if 'rule_re' in a and not re.search(a['rule_re'], x.get('rule', '')):
                return False
            if 'targets' in a and x.get('targets') != a['targets']:
                return False
            if 'min_targets' in a and x.get('targets', 0) < a['min_targets']:
                return False
            return True
        pool = [rep['assumptions'][i] for i in e['assumptions']]
        if a['kind'] == 'indirect-call':
            pool += [rep['indirect_calls'][i] for i in e.get('indirect_calls', {}).get('ids', [])]
        got = [(x.get('rule'), x.get('targets')) for x in pool if x.get('kind') == a['kind']]
        R.ok(any(ma(x) for x in pool), f"{tag}: {exp['entry'][:40]} assumption {a} (got {got[:6]})")
    for c, (lo, hi) in exp.get('count_sites', {}).items():
        sites = set()
        for f in e['findings']:
            if f['class'] != c:
                continue
            for x in [f['site']] + f.get('other_sites', []):
                sites.add((x.get('file'), x.get('line'), x.get('col')))
        R.ok(lo <= len(sites) <= hi, f"{tag}: {exp['entry'][:40]} {c} check sites {len(sites)} in [{lo},{hi}]")
    for h in exp.get('lacks', []):
        bad = [f for f in e['findings'] if all((f.get(k) == v) if k != 'via_re' else re.search(v, f.get('via', '')) for k, v in h.items())]
        R.ok(not bad, f"{tag}: {exp['entry'][:40]} has no finding {h}" + (f" (got {[(f['class'], f['symbol'], f['via']) for f in bad][:3]})" if bad else ''))
    for c in exp.get('not_plain_none', []):
        R.ok(e['verdict'].get(c) in ('reachable', 'unknown', 'none*'), f"{tag}: {exp['entry'][:40]} {c} is not a plain none (got {e['verdict'].get(c)})")
    for c in exp.get('not_none', []):
        R.ok(e['verdict'].get(c) in ('reachable', 'unknown'), f"{tag}: {exp['entry'][:40]} {c} is not none (got {e['verdict'].get(c)})")
    for k in exp.get('module_has', []):
        R.ok(any(x.get('kind') == k for x in rep['module'].get('dynamic_code', [])), f"{tag}: module lists {k}")
    if exp.get('violations') is not None:
        got = sorted(v['class'] for v in e['violations'])
        R.ok(got == sorted(exp['violations']), f"{tag}: {exp['entry'][:40]} violations {got} == {sorted(exp['violations'])}")


def check_signatures(case, eff_path, R, tag):
    F = json.load(open(eff_path))['functions']
    by = {v['demangled']: v for v in F.values()}
    for key, field in (('signature_expect', 'verdict'), ('signature_evidence', 'evidence'), ('signature_account', 'account')):
        for fn, cls in case.get(key, {}).items():
            v = by.get(fn)
            if not R.ok(v is not None, f"{tag}: function {fn} in the effect table"):
                continue
            for c, want in cls.items():
                got = v['signature'].get(c, {}).get(field)
                R.ok(got == want, f"{tag}: {fn[:50]} {c} {field} = {want} (got {got})")


PLAIN_EV = ('own', 'direct', 'indirect-pta', 'indirect-vtable', 'indirect-type')


def check_entry_vs_signature(rep, eff_path, R, tag, case):
    """The entry report is a query over the per-function table: for every entry and class the entry
    verdict and the function's signature must say the same thing (reachable = may or always; unknown =
    unknown; none or none* = never, and a plain none never rests on evidence weaker than the call-graph
    rules)."""
    if not os.path.exists(eff_path):
        return
    F = json.load(open(eff_path))['functions']
    strict = '--strict' in (case.get('extra_args') or [])
    bad = []
    n = 0
    for e in rep['entries']:
        v = F.get(e['entry'])
        if v is None or e.get('start', 'all') != 'all':
            continue
        for c, ev in e['verdict'].items():
            s = v.get('signature', {}).get(c)
            if s is None or c in ('indirect-unknown', 'external-unknown'):
                continue
            n += 1
            sv = s['verdict']
            want = {'reachable': ('may', 'always'), 'unknown': ('unknown',), 'none': ('never',), 'none*': ('never',)}[ev]
            if strict and ev == 'unknown':
                want = ('unknown', 'never')
            ok = sv in want
            if ok and ev == 'none' and s['evidence'] not in PLAIN_EV:
                ok = False
            if ok and ev == 'none*' and s['evidence'] in PLAIN_EV:
                ok = False
            if not ok:
                bad.append((e['entry'][:40], c, ev, sv, s['evidence']))
    if n:
        R.ok(not bad, f"{tag}: entry verdicts agree with the per-function signatures ({n} pairs; {bad[:4]})")


def run_hpir(case_dir, case, cflags, outdir, extra_args=()):
    os.makedirs(outdir, exist_ok=True)
    srcs = [os.path.join(case_dir, s) for s in case['sources']]
    if case.get('compile_commands'):
        srcs = ['-p', make_compile_commands(case_dir, case, outdir)]
    cmd = [sys.executable, HPIR] + srcs + ['-o', os.path.join(outdir, 'report.json'), '--text',
                                           os.path.join(outdir, 'report.txt'), '--effects', os.path.join(outdir, 'effects.json'),
                                           '--work', os.path.join(outdir, 'work'), '--path-base', case_dir, '--all', '--full',
                                           '--text-full']
    if cflags:
        cmd += ['--cflags=' + cflags]
    for e in case.get('entries', []):
        cmd += ['-e', e]
    if case.get('entries_file'):
        cmd += ['--entries-file', os.path.join(case_dir, case['entries_file'])]
    if case.get('policy'):
        pol = case['policy']
        cmd += ['--policy', os.path.join(case_dir, pol) if pol.startswith('./') else os.path.join(IR, 'policies', pol)]
    if case.get('entry_kind'):
        cmd += ['--entry-kind', case['entry_kind']]
    for s in case.get('summaries', []):
        cmd += ['--summary', os.path.normpath(os.path.join(case_dir, s))]
    cmd += list(extra_args)
    env = dict(os.environ)
    env.update(case.get('env', {}))
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if p.returncode != 0:
        return None, p.stderr + p.stdout
    return json.load(open(os.path.join(outdir, 'report.json'))), p.stderr


def make_compile_commands(case_dir, case, outdir):
    # The committed case lists commands with relative paths; the database is generated with this checkout's
    # absolute paths, and, as in a real project, the objects are first built with GCC.
    cc = case['compile_commands']
    objdir = os.path.join(outdir, 'gccobj')
    os.makedirs(objdir, exist_ok=True)
    db = []
    for c in cc['commands']:
        obj = os.path.join(objdir, os.path.basename(c['file']) + '.o')
        args = [c['compiler']] + c['args'] + [c['file'], '-o', obj]
        if cc.get('prebuild_with_gcc'):
            gargs, skip = [], False
            for a in args:  # the prebuild must not write dependency files into the case directory
                if skip:
                    skip = False
                    continue
                if a in ('-MF', '-MT', '-MQ'):
                    skip = True
                    continue
                if a in ('-MD', '-MMD') or a.startswith('-fprofile-use') or a.startswith('-flto'):
                    continue
                gargs.append(a)
            subprocess.run(gargs, cwd=case_dir, capture_output=True, text=True)
        db.append({'directory': case_dir, 'file': c['file'], 'arguments': args, 'output': obj})
    path = os.path.join(outdir, 'compile_commands.json')
    with open(path, 'w') as f:
        json.dump(db, f, indent=1)
    return path


# ------------------------------------------------------------------------------------- RealtimeSanitizer

RT_CLASS = [('alloc', r'^(malloc|calloc|realloc|reallocarray|aligned_alloc|posix_memalign|valloc|memalign|pvalloc|strdup|strndup)$'),
            ('free', r'^(free|cfree)$'),
            ('lock', r'^(pthread_|sem_|__cxa_guard)'),
            ('syscall', r'.')]


def rt_class(sym):
    for c, rx in RT_CLASS:
        if re.search(rx, sym):
            return c
    return 'syscall'


def parse_rtsan(text):
    events, cur = [], None
    for line in text.splitlines():
        m = re.search(r'ERROR: RealtimeSanitizer: (\S+)', line)
        if m:
            cur = {'kind': m.group(1), 'symbol': None, 'frames': []}
            events.append(cur)
            continue
        if cur is None:
            continue
        m = re.search(r'(?:function|call to) `([^`]+)`', line)
        if m and cur['symbol'] is None:
            cur['symbol'] = m.group(1)
            continue
        m = re.match(r'\s+#(\d+) 0x[0-9a-f]+ in (.+?)(?: /\S+:\d+(?::\d+)?)?(?: \(.*\))?$', line)
        if m:
            cur['frames'].append(m.group(2).strip())
    return events


def rtsan_compare(case_dir, case, cflags, plain_rep, outdir, R, tag, lb, extra_args=()):
    # The static report compared against RealtimeSanitizer is computed on the same build (same flags,
    # -fsanitize=realtime included): the sanitizer build keeps new/delete pairs that -O2 otherwise elides.
    rep, err = run_hpir(case_dir, case, (cflags + ' -fsanitize=realtime -DFX_RTSAN').strip(), os.path.join(outdir, 'rtsan-static'), extra_args)
    if not R.ok(rep is not None, f"{tag}: static analysis of the sanitizer build"):
        R.note(err[-800:])
        return []
    srcs = [os.path.join(case_dir, s) for s in case['sources']]
    exe = os.path.join(outdir, 'rtsan.exe')
    cmd = [os.path.join(lb, 'clang++'), '-std=c++20', '-O2', '-g', '-fsanitize=realtime', '-DFX_RTSAN', '-Wno-function-effects',
           '-w'] + shlex.split(cflags or '') + srcs + shlex.split(case.get('link_flags', '')) + ['-o', exe]
    cmd += shlex.split(case.get('rtsan_link_flags', ''))
    p = subprocess.run(cmd, capture_output=True, text=True)
    if not R.ok(p.returncode == 0, f"{tag}: rtsan build"):
        R.note(p.stderr[:400])
        return []
    env = dict(os.environ)
    env['RTSAN_OPTIONS'] = f"halt_on_error=false:symbolize=true:external_symbolizer_path={os.path.join(lb, 'llvm-symbolizer')}"
    p = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=60)
    with open(os.path.join(outdir, 'rtsan.txt'), 'w') as f:
        f.write(p.stderr)
    events = parse_rtsan(p.stderr)
    rows = []
    for ev in events:
        sym = ev['symbol'] or '?'
        cls = rt_class(sym)
        # the real-time context starts at the outermost frame that is a static entry
        ent = None
        for fr in reversed(ev['frames']):
            for e in rep['entries']:
                if fr == e['demangled'] or fr.startswith(e['demangled'].split('(')[0] + '('):
                    ent = e
                    break
            if ent:
                break
        if ent is None:
            # No entry function on the stack: the event is outside every entry. This happens when an
            # exception escapes a [[clang::nonblocking]] function: __rtsan_realtime_exit is skipped and the
            # real-time context stays on in the catch handler and later (RealtimeSanitizer limitation).
            rows.append((tag, sym, cls, '-', 'outside every entry (real-time context left on by an escaping exception)', ev['frames'][:5]))
            R.note(f"{tag}: rtsan {sym}: no entry on the stack; outside the entries' call trees")
            continue
        v = ent['verdict'].get(cls)
        frames = set(ev['frames'])
        exact = [f for f in ent['findings'] if f['class'] == cls and
                 (f['symbol'] == sym or any(fr.startswith(stripped(f['symbol'])) for fr in frames))]
        if v == 'reachable' and exact:
            status = f"covered: finding {exact[0]['symbol']} [{exact[0]['temp']}]"
        elif v == 'reachable':
            status = 'covered: class reachable (no symbol on the stack matched a finding)'
        elif v == 'unknown':
            status = 'covered by assumption (verdict unknown)'
        else:
            status = 'MISS'
        R.ok(not status.startswith('MISS'), f"{tag}: rtsan {sym} ({cls}) under {ent['demangled'][:40]}: {status}")
        pe = find_entry(plain_rep, ent['entry'])
        if pe is not None and pe['verdict'].get(cls) == 'none':
            status += '; NOT in the plain -O2 build (the call exists only in the sanitizer build)'
        rows.append((tag, sym, cls, ent['demangled'], status, ev['frames'][:5]))
    if not events:
        R.note(f"{tag}: RealtimeSanitizer reported nothing")
    return rows


_dem_cache = {}


def stripped(sym):
    if sym in _dem_cache:
        return _dem_cache[sym]
    p = subprocess.run([os.path.join(llvm_bin(), 'llvm-cxxfilt'), sym], capture_output=True, text=True)
    d = p.stdout.strip() or sym
    d = d.split('(')[0] if not d.startswith('operator') else d.split('(')[0]
    _dem_cache[sym] = d
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cases', nargs='*')
    ap.add_argument('--no-rtsan', action='store_true')
    ap.add_argument('--no-sig', action='store_true', help='skip the regression cases of tests/sig')
    ap.add_argument('--no-always', action='store_true', help='skip the run-time check of the always claims (gdb)')
    args = ap.parse_args()
    lb = llvm_bin()
    os.environ['FX_LLVM_BIN'] = lb
    names = args.cases or sorted(d for d in os.listdir(os.path.join(HERE, 'cases')) if os.path.isdir(os.path.join(HERE, 'cases', d)))
    R = Result()
    rt_rows = []
    rt_rows_comp = []  # the same comparison with points-to per component
    for name in names:
        cdir = os.path.join(HERE, 'cases', name)
        case = json.load(open(os.path.join(cdir, 'case.json')))
        runs = [('base', case.get('cflags', ''), case.get('expect', []), case.get('rtsan', True), case)]
        for v in case.get('variants', []):
            c2 = dict(case)
            c2['same_as_base'] = v.get('same_as_base', False)
            if v.get('policy_override'):
                c2['policy'] = v['policy_override']
            if v.get('env'):
                c2['env'] = v['env']
            if v.get('extra_args'):
                c2['extra_args'] = list(case.get('extra_args', [])) + v['extra_args']
            runs.append((v['name'], (case.get('cflags', '') + ' ' + v.get('cflags', '')).strip(), v.get('expect', []),
                         v.get('rtsan', case.get('rtsan', True)), c2))
        # Every case is also analysed with points-to per component (README, "Points-to per component"):
        # it must never say none where the module solve finds the class reachable, and RealtimeSanitizer
        # must not see anything it misses.
        if not case.get('compile_commands') and '--precision' not in case.get('extra_args', []):
            c3 = dict(case)
            c3['extra_args'] = list(case.get('extra_args', [])) + ['--pta-scope', 'component']
            c3['component_variant'] = True
            runs.append(('pta-component', case.get('cflags', ''), [], case.get('rtsan', True), c3))
        for vname, cflags, exps, use_rt, case in runs:
            tag = name if vname == 'base' else f'{name}[{vname}]'
            outdir = os.path.join(OUT, name if vname == 'base' else f'{name}.{vname}')
            rep, err = run_hpir(cdir, case, cflags, outdir, case.get('extra_args', []))
            if not R.ok(rep is not None, f'{tag}: fn-effects runs'):
                R.note(err[-1500:])
                continue
            R.ok(rep.get('table_agrees', False), f'{tag}: effect table agrees with the entry search')
            if vname == 'base':
                base_verdicts = {e['entry']: e['verdict'] for e in rep['entries']}
            elif case.get('component_variant'):
                weaker = [(e['entry'][:40], c, base_verdicts.get(e['entry'], {}).get(c), v) for e in rep['entries']
                          for c, v in e['verdict'].items()
                          if v in ('none', 'none*') and base_verdicts.get(e['entry'], {}).get(c) == 'reachable'
                          # a callee that may be library code is open (indirect-unknown) only in the module solve
                          and c not in ('indirect-unknown', 'external-unknown')]
                R.ok(not weaker, f'{tag}: no class is none per component where the module solve finds it reachable ({weaker[:4]})')
                R.ok(rep['module'].get('points_to', {}).get('scope', 'component') == 'component', f'{tag}: solved per component')
            elif case.get('same_as_base'):
                diffv = [(e['entry'], c) for e in rep['entries'] for c, v in e['verdict'].items()
                         if base_verdicts.get(e['entry'], {}).get(c) != v]
                R.ok(not diffv, f'{tag}: verdicts identical to the base run ({diffv[:4]})')
            for exp in exps:
                check_expect(rep, exp, R, tag)
            if vname == 'base' and (case.get('signature_expect') or case.get('signature_evidence') or case.get('signature_account')):
                check_signatures(case, os.path.join(outdir, 'effects.json'), R, tag)
            if vname != 'base':
                vdef = next((v for v in case.get('variants', []) if v['name'] == vname), None)
                if vdef and vdef.get('signature_expect'):
                    check_signatures({'signature_expect': vdef['signature_expect']}, os.path.join(outdir, 'effects.json'), R, tag)
            check_entry_vs_signature(rep, os.path.join(outdir, 'effects.json'), R, tag, case)
            eb = case.get('expect_build')
            if eb and vname == 'base':
                b = rep.get('build', {})
                R.ok(sorted(b.get('failed', [])) == sorted(eb['failed']), f"{tag}: failed translation units {b.get('failed')}")
                for d in eb.get('dropped_contains', []):
                    R.ok(d in b.get('dropped_flags', []), f"{tag}: GCC-only flag {d} reported as dropped")
                R.ok(b.get('functions_not_seen', 0) >= 1, f"{tag}: functions of the failed unit listed ({b.get('functions_not_seen')})")
            for absent in case.get('expect_absent', []) if vname == 'base' else []:
                R.ok(find_entry(rep, absent) is None or find_entry(rep, absent)['demangled'] != absent,
                     f"{tag}: {absent} is not selected as an entry")
            for chk in case.get('checks', []) if vname == 'base' else []:
                run_check(chk, cdir, case, outdir, R, tag)
            if not args.no_rtsan and use_rt:
                if case.get('component_variant'):
                    rt_rows_comp += rtsan_compare(cdir, case, cflags, rep, outdir, R, tag, lb, case['extra_args'])
                else:
                    rt_rows += rtsan_compare(cdir, case, cflags, rep, outdir, R, tag, lb)
        if 'diff' in case:
            d = case['diff']
            o, _ = run_hpir(cdir, case, d['old_cflags'], os.path.join(OUT, name + '.old'))
            n, _ = run_hpir(cdir, case, d['new_cflags'], os.path.join(OUT, name + '.new'))
            if R.ok(o is not None and n is not None, f'{name}: diff builds'):
                p = subprocess.run([sys.executable, HPIR, '--diff', os.path.join(OUT, name + '.old', 'report.json'),
                                    os.path.join(OUT, name + '.new', 'report.json')], capture_output=True, text=True)
                with open(os.path.join(OUT, name + '.diff.txt'), 'w') as f:
                    f.write(p.stdout)
                R.ok(p.returncode == d['expect_exit'], f"{name}: --diff exit {p.returncode} == {d['expect_exit']}")
                if 'expect_new_class' in d:
                    R.ok(re.search(r'^\s*\+ ' + re.escape(d['expect_new_class']) + ':', p.stdout, re.M) is not None,
                         f"{name}: --diff shows a new {d['expect_new_class']} finding")
                    R.ok(re.search(r'WORSE .*: ' + re.escape(d['expect_new_class']) + ' none -> reachable', p.stdout) is not None,
                         f"{name}: --diff shows {d['expect_new_class']} none -> reachable")
                    others = [l for l in p.stdout.splitlines() if l.startswith('  +') and d.get('expect_new_symbol', '') not in l]
                    R.ok(not others, f"{name}: every new finding comes from the injected call {d.get('expect_new_symbol')} ({others[:3]})")
    always_line = ''
    if not args.no_always:
        # Run-time check of every `always` claim (README, "Per-function signatures"): tests/validate_always.py
        import validate_always
        if 'always_rules' in names:
            # negative control: false claims the checker must refute (two always, one never)
            t2, l2 = validate_always.main(['always_rules'], quiet=True, inject=[('_Z10one_branchl', 'alloc', 'always'),
                                          ('_Z9reserve_nl', 'alloc', 'always'), ('_Z10every_pathl', 'alloc', 'never')])
            nv = t2.get(('.module', 'O0'), [0] * 5)[4]
            R.ok(nv >= 3, f"always: the run-time check refutes three injected false claims (counterexamples {nv})")
        tot, lines = validate_always.main(names, quiet=True)
        parts = []
        for (sub, build), t in sorted(tot.items()):
            R.ok(t[4] == 0, f"run-time check, {sub[1:]} table, {build} build: no counterexample ({t[2]} always and {t[3]} never claims on "
                            f"{t[0]} functions, {t[1]} functions run, {t[4]} counterexamples)")
            parts.append(f"{sub[1:]}/{build}: {t[2]} always + {t[3]} never claims, {t[1]} of {t[0]} functions run, {t[4]} counterexamples")
            for l in lines:
                if 'VIOLATION' in l and t[4]:
                    R.note(l)
        always_line = '; run-time check ' + '; '.join(parts)
    sig_line = ''
    if not args.no_sig and not args.cases:
        # the adversary's attempts of round 6, as regression cases (tests/sig/<attempt>/run.sh, expect.txt)
        sigdir = os.path.join(HERE, 'sig')
        nsig = 0
        for d in sorted(os.listdir(sigdir)):
            rs = os.path.join(sigdir, d, 'run.sh')
            if not os.path.exists(rs):
                continue
            nsig += 1
            try:
                p = subprocess.run(['bash', rs], capture_output=True, text=True, timeout=1200)
                out = p.stdout + p.stderr
            except subprocess.TimeoutExpired:
                out = 'TIMEOUT'
            open(os.path.join(sigdir, d, 'out.txt'), 'w').write(out)
            bad = [l for l in out.splitlines() if re.search(r'^VIOLATION|fn-effects FAILED|GDB-PROBLEM|TIMEOUT|^INJECTED-CLAIM-PASSED|^FALSE-COUNTEREXAMPLE', l)]
            R.ok(not bad, f"sig/{d}: no run-time counterexample to a claim ({bad[:2]})")
            ex = os.path.join(sigdir, d, 'expect.txt')
            for l in open(ex).read().splitlines() if os.path.exists(ex) else []:
                if not l.strip() or l.startswith('#'):
                    continue
                neg = l.startswith('!')
                rx = l[1:] if neg else l
                found = re.search(rx, out, re.M) is not None
                R.ok(found != neg, f"sig/{d}: {'absent' if neg else 'present'}: {rx[:90]}")
        sig_line = f'; adversary regression cases: {nsig}'
    with open(os.path.join(OUT, 'rtsan-comparison.txt'), 'w') as f:
        for r in rt_rows:
            f.write(f"{r[0]:<34} {r[1]:<22} {r[2]:<8} {r[3][:48]:<48} {r[4]}\n          stack: {' < '.join(r[5])}\n")
    with open(os.path.join(OUT, 'results.txt'), 'w') as f:
        f.write('\n'.join(R.lines) + '\n')
    print('\n'.join(R.lines))
    npass = sum(1 for l in R.lines if l.startswith('PASS'))
    print(f'\n{npass} passed, {R.fail} failed; RealtimeSanitizer events: {len(rt_rows)}, '
          f'misses: {sum(1 for r in rt_rows if r[4].startswith("MISS"))}; with points-to per component: events '
          f'{len(rt_rows_comp)}, misses: {sum(1 for r in rt_rows_comp if r[4].startswith("MISS"))}{always_line}{sig_line}')
    sys.exit(1 if R.fail else 0)


def run_check(chk, cdir, case, outdir, R, tag):
    """Extra checks: baseline, check exit status, relative work directory."""
    kind = chk['kind']
    if kind == 'relative_work':
        # regression (round 4): a relative --work must work although each compile runs in its own directory
        os.makedirs(outdir, exist_ok=True)
        db = make_compile_commands(cdir, case, outdir)
        p = subprocess.run([sys.executable, HPIR, '-p', db, '--work', 'relwork', '-o', 'rel.json', '--text', 'rel.txt',
                            '--entries-file', os.path.join(cdir, case['entries_file']), '--path-base', cdir], cwd=outdir,
                           capture_output=True, text=True)
        R.ok(p.returncode == 0 and os.path.exists(os.path.join(outdir, 'rel.json')), f'{tag}: relative --work directory')
        return
    if kind == 'baseline':
        bl = os.path.join(outdir, 'baseline.json')
        rep, err = run_hpir(cdir, case, '', os.path.join(outdir, 'b1'), ['--write-baseline', bl])
        R.ok(rep is not None and os.path.exists(bl), f'{tag}: baseline written')
        srcs = [os.path.join(cdir, s) for s in case['sources']]
        cmd = [sys.executable, HPIR] + srcs + ['-o', os.path.join(outdir, 'b2.json'), '--text', os.path.join(outdir, 'b2.txt'),
                                               '--baseline', bl, '--check', '--path-base', cdir]
        for e in case.get('entries', []):
            cmd += ['-e', e]
        for s2 in case.get('summaries', []):
            cmd += ['--summary', os.path.normpath(os.path.join(cdir, s2))]
        p = subprocess.run(cmd, capture_output=True, text=True)
        R.ok(p.returncode == 0, f'{tag}: --check with the baseline of the same build exits 0 (got {p.returncode})')
        cmd2 = cmd[:]
        cmd2[cmd2.index('--baseline') + 1] = os.path.join(outdir, 'nonexistent.json')
        open(cmd2[cmd2.index('--baseline') + 1], 'w').write('{"accepted": []}')
        p = subprocess.run(cmd2, capture_output=True, text=True)
        R.ok(p.returncode == 1, f'{tag}: --check with an empty baseline exits 1 (got {p.returncode})')


if __name__ == '__main__':
    main()
