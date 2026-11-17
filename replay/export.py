#!/usr/bin/env python3
"""Write results/<build>/<project>.csv from out/: one row per measured commit and operation (or one row
per commit that failed to build or run), in first-parent order.

  export.py [BUILD ...]        default: every directory under out/
"""
import csv, json, sys
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent

def main():
    builds = sys.argv[1:] or sorted(p.name for p in (TOP / "out").iterdir() if p.is_dir())
    for b in builds:
        for proj in sorted(p for p in (TOP / "out" / b).iterdir() if p.is_dir()):
            order = proj / "order.json"
            if not order.exists(): continue
            dst = TOP / "results" / b / f"{proj.name}.csv"; dst.parent.mkdir(parents=True, exist_ok=True)
            n = 0
            with open(dst, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["commit", "date", "status", "op", "alloc_per_call", "free_per_call", "lock_per_call",
                            "first_call_alloc", "first_call_lock", "median_ns", "subject"])
                for h, date, subj in json.load(open(order)):
                    res, fail = proj / f"{h}.jsonl", proj / f"{h}.fail"
                    if res.exists():
                        for d in map(json.loads, res.read_text().splitlines()):
                            w.writerow([h, date, "ok", d["op"], d["alloc"], d["free"], d["lock"], d.get("first_alloc", ""),
                                        d.get("first_lock", ""), d["ns"], subj]); n += 1
                    elif fail.exists():
                        w.writerow([h, date, "build failed" if fail.read_text().startswith("BUILD") else "run failed",
                                    "", "", "", "", "", "", "", subj]); n += 1
            print(f"{dst.relative_to(TOP)}: {n} rows")

if __name__ == "__main__":
    main()
