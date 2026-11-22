#!/usr/bin/env python3
"""Commits where an operation's exact counts changed, from replay.py output.

  diff.py PROJECT [--cc gcc] [--tag TAG] [--ns PCT] [--instr [PCT]]

A change is a difference of at least 0.01 per call in allocations, frees or mutex locks, or any
difference in the counts of the first call. With --ns, also median-time changes of at least PCT percent
(indicative only when the runs were made in parallel). With --instr, the output of replay.py --metric
instr is read instead, and a change is a difference of at least PCT percent (default 3) in instructions
per call; instruction counts are exact, so every change reported is real, whether or not it costs time.
"""
import json, sys
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay as rp

def main():
    proj = sys.argv[1]
    cc = sys.argv[sys.argv.index("--cc") + 1] if "--cc" in sys.argv else "gcc"
    tag = sys.argv[sys.argv.index("--tag") + 1] if "--tag" in sys.argv else ""
    pct = float(sys.argv[sys.argv.index("--ns") + 1]) if "--ns" in sys.argv else None
    instr = "--instr" in sys.argv
    ipct = 3.0
    if instr:
        k = sys.argv.index("--instr")
        if k + 1 < len(sys.argv) and sys.argv[k + 1].replace(".", "", 1).isdigit(): ipct = float(sys.argv[k + 1])
    out = TOP / "out" / (cc + tag + ("-instr" if instr else "")) / proj
    fields = rp.compared_fields(proj) if proj in rp.P else ("alloc", "free", "lock", "first_alloc", "first_lock")
    order = json.load(open(out / "order.json"))
    prev = None; n_ok = n_bfail = n_rfail = n_chg = 0; first = last = None
    idx = {c: i for i, (c, _, _) in enumerate(order)}
    for h, date, subj in order:
        f = out / f"{h}.jsonl"
        if not f.exists():
            t = (out / f"{h}.fail").read_text() if (out / f"{h}.fail").exists() else ""
            n_bfail += t.startswith("BUILD"); n_rfail += t.startswith("RUN"); continue
        n_ok += 1; first = first or (h, date); last = (h, date)
        cur = {d["op"]: d for d in map(json.loads, f.read_text().splitlines())}
        if prev:
            ph, pcur = prev; lines = []
            for op in cur:
                if op not in pcur: continue
                a, b = pcur[op], cur[op]
                if a.get("exact", True) is False or b.get("exact", True) is False:
                    continue   # several threads (bench.py): not compared
                if instr:
                    if abs(a["instr"] - b["instr"]) * 100 >= ipct * max(a["instr"], 1):
                        lines.append(f"    {op}: instructions {a['instr']:.0f} -> {b['instr']:.0f} ({(b['instr'] - a['instr']) * 100 / a['instr']:+.0f}%)")
                    continue
                ch = [f"{k} {a[k]:.2f} -> {b[k]:.2f}" for k in ("alloc", "free", "lock") if k in fields and abs(a[k] - b[k]) >= 0.01]
                ch += [f"{k} {a[k]} -> {b[k]}" for k in ("first_alloc", "first_lock") if k in a and k in b and a[k] != b[k]]
                if pct and a["ns"] and abs(b["ns"] - a["ns"]) * 100 >= pct * a["ns"]:
                    ch.append(f"ns {a['ns']:.0f} -> {b['ns']:.0f}")
                if ch: lines.append(f"    {op}: " + "; ".join(ch))
            if lines:
                n_chg += 1
                gap = idx[h] - idx[ph] - 1
                if gap == 0:
                    print(f"{h[:8]} {date} {subj[:80]}   (from {ph[:8]}, the previous library commit)")
                else:   # say why the commits in between were not measured: the change is anywhere in them
                    why = {"failed to build": 0, "failed to run": 0, "not run": 0}
                    for c, _, _ in order[idx[ph] + 1:idx[h]]:
                        fl = out / f"{c}.fail"
                        why["not run" if not fl.exists() else "failed to build" if fl.read_text().startswith("BUILD") else "failed to run"] += 1
                    reason = ", ".join(f"{n} {k}" for k, n in why.items() if n)
                    print(f"{ph[:8]}..{h[:8]} {date} {subj[:60]}   ({gap} commits in between not measured: {reason};"
                          f" the change is in this interval{'; run --refine' if why['not run'] else ''})")
                print("\n".join(lines))
        prev = (h, cur)
    if "free" not in fields: print(f"(frees not compared: the workload runs another thread, see replay.compared_fields)")
    print(f"{proj}: {len(order)} library commits; measured {n_ok} (from {first[1] if first else '-'} to {last[1] if last else '-'}), "
          f"build failed {n_bfail}, run failed {n_rfail}; {n_chg} measured commits changed {'instructions' if instr else 'a count'}")
    if first and last and first[0] != last[0]:   # the net change over the whole measured history
        f0 = {d["op"]: d for d in map(json.loads, (out / f"{first[0]}.jsonl").read_text().splitlines())}
        f1 = {d["op"]: d for d in map(json.loads, (out / f"{last[0]}.jsonl").read_text().splitlines())}
        keys = ("instr",) if instr else tuple(k for k in ("alloc", "lock") if k in fields)
        net = [f"{op}: " + ", ".join(f"{k} {f0[op][k]:.{0 if instr else 2}f} -> {f1[op][k]:.{0 if instr else 2}f}" for k in keys)
               for op in f1 if op in f0 and all(k in f0[op] and k in f1[op] for k in keys)]
        if net: print(f"  net, {first[1]} to {last[1]}: " + "; ".join(net))
    if last and instr:
        print("  latest measured: " + "; ".join(f"{d['op']}: {d['instr']:.0f} instructions"
              for d in map(json.loads, (out / f"{last[0]}.jsonl").read_text().splitlines())))
    elif last:
        print("  latest measured: " + "; ".join(f"{d['op']}: alloc {d['alloc']:.2f} lock {d['lock']:.2f} (first call: "
              f"{d.get('first_alloc', '?')} alloc, {d.get('first_lock', '?')} lock)"
              for d in map(json.loads, (out / f"{last[0]}.jsonl").read_text().splitlines())))

if __name__ == "__main__":
    main()
