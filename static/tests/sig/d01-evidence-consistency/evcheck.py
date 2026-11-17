#!/usr/bin/env python3
"""evcheck.py effects.json...: is a `never` with evidence own/direct backed by the table itself?
own    -> the function has no call edge at all and no own site that is a call of an external
direct -> every edge of its closure is kind 0 (direct), no own site anywhere in the closure comes from a library
Also: always => rests_on contains the kinds of the edges it uses (spot check: an `always` with rests_on lacking any indirect kind while
every always-callee is reached only through an indirect edge)."""
import json, sys, collections
tot = collections.Counter()
for path in sys.argv[1:]:
    try: E = json.load(open(path))['functions']
    except Exception as e: continue
    memo = {}
    def closure(f):
        seen, st = {f}, [f]
        kinds = set(); ext = set()
        while st:
            g = st.pop()
            for c in E.get(g, {}).get('calls', []):
                kinds.add(c.get('kind'))
                t = c['callee']
                if t not in E: ext.add(t)
                elif t not in seen: seen.add(t); st.append(t)
        return seen, kinds, ext
    for f, v in E.items():
        sig = v.get('signature', {})
        evs = {c: s.get('evidence') for c, s in sig.items() if s.get('verdict') == 'never'}
        if not any(e in ('own', 'direct') for e in evs.values()): continue
        seen, kinds, ext = closure(f)
        tot['never own/direct functions'] += 1
        own_sites = [(g, s.get('symbol'), s.get('class'), s.get('via')) for g in seen for s in E[g].get('own', []) ]
        libsites = [x for x in own_sites if x[3] not in (None, 'instruction')]
        for c, e in evs.items():
            if e == 'own' and (v.get('calls')):
                tot['own-with-calls'] += 1
                if tot['own-with-calls'] <= 8: print(f"MISLABEL? {path}: {f} never {c} [own] but it has call edges: {[(x['callee'], x['kind']) for x in v['calls']][:3]}")
                break
        if kinds - {0}:
            tot['direct-with-indirect-edge'] += 1
            if tot['direct-with-indirect-edge'] <= 8: print(f"MISLABEL? {path}: {f} never [own/direct] {sorted(set(evs.values()))} but closure has edge kinds {kinds}")
        if ext:
            tot['closure-with-external'] += 1
            if tot['closure-with-external'] <= 8: print(f"MISLABEL? {path}: {f} never [own/direct] but closure calls externals {sorted(ext)[:4]}")
        if libsites:
            tot['closure-with-library-site'] += 1
            if tot['closure-with-library-site'] <= 12: print(f"NOTE {path}: {f} never[own/direct] for {[c for c,e in evs.items() if e in ('own','direct')][:6]}; closure has library sites {sorted(set((s[1], s[2]) for s in libsites))[:6]}")
print(dict(tot))
