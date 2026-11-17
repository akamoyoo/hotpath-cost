#!/usr/bin/env python3
"""Put a price on a change: time two commits of a library alternately on one core.

  price.py PROJECT A B [--cc gcc|gcc15|clang] [--rounds N] [--core C]

Builds the workload at commits A and B, then runs them alternately (A, B, A, B, ...) for N rounds
(default 5) pinned to one core, and prints per operation the median ns of each, the spread between
rounds, and the ratio B/A. Run it on an otherwise idle machine, on a core whose hyperthread sibling is
idle: the counts and instruction metrics find a change; this tells whether it costs time.
"""
import json, statistics, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay as rp

def busy(cores, seconds=1.0):
    """Fraction of time each core (and its hyperthread siblings) was busy over a short sample, from /proc/stat."""
    import time
    def sample():
        out = {}
        for l in open("/proc/stat"):
            if l.startswith("cpu") and l[3].isdigit():
                f = l.split(); v = list(map(int, f[1:])); out[int(f[0][3:])] = (sum(v), v[3] + v[4])
        return out
    a = sample(); time.sleep(seconds); b = sample()
    return {c: 1 - (b[c][1] - a[c][1]) / max(1, b[c][0] - a[c][0]) for c in cores if c in a}

def siblings(core):
    try:
        txt = open(f"/sys/devices/system/cpu/cpu{core}/topology/thread_siblings_list").read().strip()
    except OSError: return [core]
    out = []
    for part in txt.split(","):
        lo, _, hi = part.partition("-"); out += list(range(int(lo), int(hi or lo) + 1))
    return out

def check_idle(cores):
    """Warn when a core used for timing, or its hyperthread sibling, is busy: the times would be noise."""
    allc = sorted({s for c in cores for s in siblings(c)})
    hot = {c: u for c, u in busy(allc).items() if u > 0.05}
    if hot:
        print("warning: busy " + ", ".join(f"cpu{c} {u:.0%}" for c, u in hot.items()) +
              " (timing cores and their hyperthread siblings should be idle)", file=sys.stderr)

def main():
    proj, a, b = sys.argv[1:4]
    cc = sys.argv[sys.argv.index("--cc") + 1] if "--cc" in sys.argv else "gcc"
    rounds = int(sys.argv[sys.argv.index("--rounds") + 1]) if "--rounds" in sys.argv else 5
    core = sys.argv[sys.argv.index("--core") + 1] if "--core" in sys.argv else str(rp.CORES[0])
    cores = ",".join([core] + [str(c) for c in rp.CORES if str(c) != core][:rp.P[proj].get("cores", 1) - 1])
    full = lambda c: subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", c], capture_output=True, text=True, check=True).stdout.strip()
    res = {}
    check_idle([int(c) for c in cores.split(",")])
    with tempfile.TemporaryDirectory() as t:
        t = Path(t); exes = {}
        for c in (a, b):
            d = t / c; d.mkdir(); exe = d / "x"
            r = rp.build(proj, cc, full(c), d, exe)
            if r.returncode: sys.exit(f"{c}: build failed\n{r.stderr[:2000]}")
            exes[c] = exe
        check_idle([int(c) for c in cores.split(",")])
        for _ in range(rounds):
            for c, exe in exes.items():
                r = subprocess.run(["taskset", "-c", cores, str(exe)], capture_output=True, text=True)
                if r.returncode: sys.exit(f"{c}: the workload failed (exit {r.returncode})\n{r.stderr[-2000:]}")
                out = r.stdout
                for d in (json.loads(l) for l in out.splitlines() if l.startswith('{"op"')):
                    res.setdefault(d["op"], {}).setdefault(c, []).append(d["ns"])
    print(f"{proj} {cc} {rp.OPT}: {a} -> {b}, median ns of {rounds} alternating rounds on core {core}")
    for op, v in res.items():
        ma, mb = statistics.median(v[a]), statistics.median(v[b])
        sa, sb = max(v[a]) - min(v[a]), max(v[b]) - min(v[b])
        print(f"  {op[:40]:40} {ma:8.1f} (±{sa / 2:.1f})  {mb:8.1f} (±{sb / 2:.1f})   x{mb / ma:.2f}")

if __name__ == "__main__":
    main()
