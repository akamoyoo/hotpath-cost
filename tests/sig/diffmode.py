#!/usr/bin/env python3
"""diffmode.py MODULE_effects.json COMPONENT_effects.json [label]: verdicts where component mode is stronger than module mode."""
import json, sys
A = json.load(open(sys.argv[1]))['functions']; B = json.load(open(sys.argv[2]))['functions']
lab = sys.argv[3] if len(sys.argv) > 3 else ''
n = 0
for f in A:
    if f not in B: continue
    sa, sb = A[f].get('signature', {}), B[f].get('signature', {})
    for c in sa:
        va, vb = sa[c].get('verdict'), sb.get(c, {}).get('verdict')
        if va == vb: continue
        bad = (vb == 'never' and va in ('may', 'always', 'unknown')) or (vb == 'always' and va != 'always')
        if bad:
            n += 1
            print(f"DIFF {lab} {f} {c}: module={va}[{sa[c].get('evidence')}] component={vb}[{sb[c].get('evidence')}]")
print(f"diffmode {lab}: {len(A)} functions, {n} stronger-in-component verdicts")
