#!/usr/bin/env python3
"""Check a change for hot-path costs: counts, then stacks, then instructions, at two commits.

  check.py PROJECT BASE HEAD [--cc gcc|gcc15|clang] [--no-instr] [--instr-pct PCT]

Builds the workload at BASE and HEAD (any two commits of the library's clone in src/, for example a
merge base and a branch head), and reports:

  1. allocations, frees and mutex locks per call of every operation, steady state and first call;
  2. if any of them changed, the stacks that were added, removed or changed (as explain.py);
  3. instructions per call under callgrind, if VALGRIND is set (as replay.py --metric instr), with the
     changes of PCT percent or more (default 3).

Counts and instructions are exact. Times are not reported: price a change with price.py.
Exits 1 if a count changed, 2 if only instructions changed, 3 if a build or run failed, 0 otherwise.
"""
import json, os, subprocess, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import replay as rp

def fail(msg):
    print(msg, file=sys.stderr); sys.exit(3)

def measure(proj, cc, h, instr):
    """One run of the workload at commit h: {op: record}."""
    with tempfile.TemporaryDirectory() as t:
        t = Path(t); exe = t / "_workload_exe"
        extra = ("-DREPLAY_INSTR", *([f"-I{rp.VALGRIND_INC}"] if rp.VALGRIND_INC else [])) if instr else ()
        b = rp.build(proj, cc, h, t, exe, extra=extra)
        if b.returncode: fail(f"{h[:8]}: build failed\n{b.stderr[-3000:]}")
        cores = ",".join(map(str, rp.CORES[:rp.P[proj].get("cores", 1)]))
        cmd = ["taskset", "-c", cores, *([rp.VALGRIND, "--tool=callgrind", f"--callgrind-out-file={t}/cg.out",
                                          "--dump-instr=no", "--compress-strings=no", "--compress-pos=no"] if instr else []), str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        if r.returncode: fail(f"{h[:8]}: the workload failed (exit {r.returncode})\n{r.stderr[-3000:]}")
        if instr: return {d["op"]: d for d in map(json.loads, rp.instr_lines(t, r.stdout))}
        return {d["op"]: d for d in (json.loads(l) for l in r.stdout.splitlines() if l.startswith('{"op"'))}

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cc = sys.argv[sys.argv.index("--cc") + 1] if "--cc" in sys.argv else "gcc"
    pct = float(sys.argv[sys.argv.index("--instr-pct") + 1]) if "--instr-pct" in sys.argv else 3.0
    args = [a for a in args if a not in (cc, str(pct))]
    proj, base, head = args[:3]
    rev = lambda c: subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", c],
                                   capture_output=True, text=True, check=True).stdout.strip()
    b, h = rev(base), rev(head)
    if b == h and "--same-ok" not in sys.argv:   # comparing a commit with itself always reports no change
        fail(f"{base} and {head} are the same commit ({b[:8]})")
    fields = rp.compared_fields(proj)
    print(f"{proj} {cc} {rp.OPT}: {b[:8]} -> {h[:8]}")
    before, after = measure(proj, cc, b, False), measure(proj, cc, h, False)
    changed = False
    print("counts per call (steady state; first call in brackets):")
    for op in after:
        x, y = before.get(op), after[op]
        if x is None: print(f"  {op}: new operation"); continue
        diff = [k for k in ("alloc", "free", "lock") if k in fields and abs(x[k] - y[k]) >= 0.01]
        diff += [k for k in ("first_alloc", "first_lock") if x.get(k) != y.get(k)]
        changed |= bool(diff)
        show = lambda d: f"alloc {d['alloc']:.2f} lock {d['lock']:.2f} [{d.get('first_alloc')}, {d.get('first_lock')}]"
        print(f"  {op:40} {show(x)}  ->  {show(y)}" + ("   CHANGED" if diff else ""))
    if changed:
        print("\nstacks (explain.py):")
        r = subprocess.run([sys.executable, str(HERE / "explain.py"), proj, h, b, "--cc", cc], capture_output=True, text=True)
        print("\n".join(l for l in (r.stdout + r.stderr).splitlines()[1:]))
    ichanged = False
    if "--no-instr" not in sys.argv and rp.VALGRIND:
        print("\ninstructions per call (callgrind):")
        # with another thread (cores > 1), the calling thread's instructions depend on scheduling (Taskflow:
        # +-2 % between runs of one build): measure three times and call a change only if the ranges are apart
        reps = 3 if rp.P[proj].get("cores", 1) > 1 else 1
        if reps > 1: print(f"  (the workload runs another thread: {reps} runs each, min..max)")
        bi = [measure(proj, cc, b, True) for _ in range(reps)]; hi = [measure(proj, cc, h, True) for _ in range(reps)]
        for op in hi[0]:
            if op not in bi[0]: continue
            xs, ys = [r[op]["instr"] for r in bi], [r[op]["instr"] for r in hi]
            x, y = sorted(xs)[len(xs) // 2], sorted(ys)[len(ys) // 2]
            d = (y - x) * 100 / max(x, 1)
            apart = min(ys) > max(xs) or max(ys) < min(xs)
            ch = abs(d) >= pct and (reps == 1 or apart)
            ichanged |= ch
            rng = lambda v: f"{v[0]:.0f}" if reps == 1 else f"{min(v):.0f}..{max(v):.0f}"
            print(f"  {op:40} {rng(xs):>12} -> {rng(ys):>12}  ({d:+.1f} %)" + ("   CHANGED" if ch else ""))
    elif "--no-instr" not in sys.argv:
        print("\n(instructions not measured: set VALGRIND, and VALGRIND_INC for callgrind.h)")
    verdict = ("a count changed: name the stacks above, and price it with price.py" if changed else
               "no count changed; instructions changed: price it with price.py before calling it a cost" if ichanged else
               "no count and no instruction count changed")
    print(f"\nverdict: {verdict}")
    sys.exit(1 if changed else 2 if ichanged else 0)

if __name__ == "__main__":
    main()
