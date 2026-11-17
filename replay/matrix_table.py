#!/usr/bin/env python3
"""Table of the compiler matrix (replay/matrix.sh output): median ns per call of every operation at the
latest library commit, per compiler and level, with the ratio of the slowest to the fastest build.
Counts (allocations, locks per call) are printed when they differ between builds.

  matrix_table.py [--out DIR] [--markdown]
"""
import json, statistics, sys
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
BUILDS = [("gcc", "-O2", "GCC 13 -O2"), ("gcc", "-O3", "GCC 13 -O3"), ("gcc15", "-O2", "GCC 15 -O2"),
          ("gcc15", "-O3", "GCC 15 -O3"), ("clang", "-O2", "clang 21 -O2"), ("clang", "-O3", "clang 21 -O3")]
PROJECTS = ["liquibook", "concurrentqueue", "atomic_queue", "SPSCQueue", "MPMCQueue", "hffix", "spdlog", "fmt", "simdjson", "quill",
            "readerwriterqueue", "unordered_dense", "robin-map", "parallel-hashmap", "robin-hood-hashing", "fast_float", "xxHash", "eigen"]

def main():
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else TOP / "out"
    md = "--markdown" in sys.argv
    if md:
        print("| library | operation | " + " | ".join(b[2] for b in BUILDS) + " | slowest / fastest |")
        print("|---|---|" + "---:|" * (len(BUILDS) + 1))
    for p in PROJECTS:
        runs = {}
        for cc, o, name in BUILDS:
            reps = []   # one result per round (out/<cc>-m<opt>-r<k>/), or a single unrounded one
            for d in sorted(out.glob(f"{cc}-m{o}-r*")) + [out / f"{cc}-m{o}"]:
                fs = sorted((d / p).glob("*.jsonl")) if (d / p).exists() else []
                if fs: reps.append({r["op"]: r for r in map(json.loads, fs[-1].read_text().splitlines())})
            if reps:
                med = {}
                for op in reps[0]:
                    vals = [r[op] for r in reps if op in r]
                    med[op] = dict(vals[0], ns=statistics.median(v["ns"] for v in vals), n=len(vals),
                                   spread=max(v["ns"] for v in vals) - min(v["ns"] for v in vals))
                runs[name] = med
        if not runs: continue
        ops = list(next(iter(runs.values())))
        for op in ops:
            ns = [runs[b[2]][op]["ns"] if b[2] in runs and op in runs[b[2]] else None for b in BUILDS]
            have = [x for x in ns if x]
            ratio = max(have) / min(have) if have else 0
            counts = {(runs[b[2]][op]["alloc"], runs[b[2]][op]["lock"]) for b in BUILDS if b[2] in runs and op in runs[b[2]]}
            note = "" if len(counts) <= 1 else "  counts differ: " + ", ".join(
                f"{b[2]} alloc {runs[b[2]][op]['alloc']:.2f} lock {runs[b[2]][op]['lock']:.2f}" for b in BUILDS if b[2] in runs)
            cells = [f"{x:.0f}" if x else "-" for x in ns]
            if md:
                print(f"| {p} | {op} | " + " | ".join(cells) + f" | {ratio:.2f} |" + (f" {note}" if note else ""))
            else:
                print(f"{p:16} {op[:34]:34} " + " ".join(f"{c:>7}" for c in cells) + f"  x{ratio:.2f}{note}")

if __name__ == "__main__":
    main()
