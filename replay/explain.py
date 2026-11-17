#!/usr/bin/env python3
"""Explain a count change: which allocation or lock stacks appeared, disappeared or changed rate between
two commits.

  explain.py PROJECT COMMIT [BASE] [--cc gcc|gcc15|clang] [--all]

BASE defaults to the measured commit before COMMIT in replay.py's results (the commit diff.py reports the
change "from"), or else COMMIT's first parent. Both are built with call-stack recording (as attribute.py
does) and their stacks are compared per operation and phase. Stacks are compared by the names of their
library functions, not by line numbers, which move between commits; each is printed with its innermost
library frame at both commits. Only stacks whose count per call differs are printed, unless --all.
"""
import json, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay as rp
import attribute as at

def library_frames(frames):
    """The frames that belong to the library: not the C++ runtime, not the workload or its driver."""
    out = []
    for fn, loc in frames:
        if any(s in loc for s in at.SKIP) or fn.startswith(("operator new", "operator delete", "std::", "__gnu_cxx", "malloc")):
            continue
        if loc.startswith("replay/"): break   # the workload and the entry functions: the stack's root
        out.append((fn, loc))
    return out

def short(fn, n=90):
    return fn if len(fn) <= n else fn[:n - 3] + "..."

def stacks(events):
    """{(op, phase, kind, function names): (per_call, innermost library frame)}; equal keys are summed."""
    out = {}
    for e in events:
        lib = library_frames(e["frames"])
        key = (e["op"], e["phase"], e["kind"], tuple(fn for fn, _ in lib))
        if not lib:   # the library frames were lost (inlined without debug records): name the entry instead
            rest = [(fn, loc) for fn, loc in e["frames"] if not any(x in loc for x in at.SKIP)
                    and not fn.startswith(("operator new", "std::", "__gnu_cxx", "malloc"))]
            lib_top = (f"(no library frame) {rest[0][0]}", rest[0][1]) if rest else ("(no library frame)", "")
        pc, top = out.get(key, (0.0, None))
        out[key] = (pc + e["per_call"], top or (lib[0] if lib else lib_top))
    return out

def base_of(proj, cc, h):
    order = rp.outdir(proj, cc) / "order.json"
    if order.exists():
        prev = None
        for c, _, _ in json.load(open(order)):
            if c == h: break
            if (rp.outdir(proj, cc) / f"{c}.jsonl").exists(): prev = c
        else: prev = None
        if prev: return prev
    return subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", h + "^1"],
                          capture_output=True, text=True, check=True).stdout.strip()

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cc = sys.argv[sys.argv.index("--cc") + 1] if "--cc" in sys.argv else "gcc"
    if cc in args: args.remove(cc)
    proj, h = args[0], args[1]
    rev = lambda c: subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", c],
                                   capture_output=True, text=True, check=True).stdout.strip()
    h = rev(h); b = rev(args[2]) if len(args) > 2 else base_of(proj, cc, h)
    try:
        before, after = stacks(at.trace(proj, b, cc)), stacks(at.trace(proj, h, cc))
    except RuntimeError as e:
        sys.exit(str(e))
    print(f"{proj} {cc}: {b[:8]} -> {h[:8]}")
    every = "--all" in sys.argv
    total = lambda st, op, ph, k: sum(v[0] for key, v in st.items() if key[:3] == (op, ph, k))
    def lines(op, ph):
        """The report of one operation and phase: totals per kind, then the stacks that changed."""
        out = []
        for kind in ("alloc", "lock"):
            x, y = total(before, op, ph, kind), total(after, op, ph, kind)
            if abs(x - y) >= 0.01 or every: out.append(f"  {kind:5} {x:6.2f} -> {y:6.2f} per call in total")
        for key in sorted(k for k in set(before) | set(after) if k[:2] == (op, ph)):
            x, y = before.get(key, (0.0, None)), after.get(key, (0.0, None))
            if abs(x[0] - y[0]) < 0.01 and not every: continue
            tag = "new" if x[1] is None else "gone" if y[1] is None else "changed"
            out.append(f"    {key[2]:5} {x[0]:6.2f} -> {y[0]:6.2f}  ({tag})")
            for label, top in (("before", x[1]), ("after", y[1])):
                if top: out.append(f"        {label:6} {short(top[0])}  {top[1]}")
            out += [f"          via  {short(fn)}" for fn in key[3][1:4]]
        return out
    n = 0
    for op in dict.fromkeys(k[0] for k in list(before) + list(after)):
        first, steady = lines(op, "first call"), lines(op, "steady state")
        if first == steady:
            if steady: print(f"== {op}, first call and steady state"); print("\n".join(steady)); n += 1
            continue
        for ph, ls in (("first call", first), ("steady state", steady)):
            if ls: print(f"== {op}, {ph}"); print("\n".join(ls)); n += 1
    if not n: print("  no operation changed its counts")
    if n: print("Stacks are matched by function names; a refactoring that renames them shows as a stack that is\n"
                "gone and one that is new, with the totals unchanged.")

if __name__ == "__main__":
    main()
