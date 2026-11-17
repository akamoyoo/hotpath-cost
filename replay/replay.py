#!/usr/bin/env python3
"""Replay a library's history with exact per-operation counts.

At first-parent commits that changed the library, build replay/bench/<bench>.cpp against that commit
and run it. Each run reports, per operation: heap allocations, frees and mutex locks per call (exact,
counted on the calling thread), the same for the very first call, and the median ns per call.

  replay.py PROJECT                      every library commit
  replay.py PROJECT --stride K           every K-th commit (and the last)
  replay.py PROJECT --refine             bisect every interval whose counts differ, down to one commit
  replay.py PROJECT --commits A,B        only these commits (prefixes)
  options: --cc gcc|gcc15|clang  -j N
  environment: CXX_GCC, CXX_GCC15, CXX_CLANG (compilers), REPLAY_OPT (default -O2),
               REPLAY_TAG (suffix of the output directory), REPLAY_CORES (cores to pin runs to)

Output: out/<cc><tag>/<project>/<commit>.jsonl, or <commit>.fail with the build or run error.
Projects are expected under src/<project> (fetch.sh clones them). Counts do not depend on machine
load; timings of parallel runs do, and are only indicative.
"""
import concurrent.futures, json, os, subprocess, sys, tempfile, threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOP = HERE.parent
CXX = {"gcc": os.environ.get("CXX_GCC", "g++"), "gcc15": os.environ.get("CXX_GCC15", "g++-15"),
       "clang": os.environ.get("CXX_CLANG", "clang++")}
OPT = os.environ.get("REPLAY_OPT", "-O2")
TAG = os.environ.get("REPLAY_TAG", "" if OPT == "-O2" else OPT)

# drv: replay/drivers/<drv>/main.cpp holds the entry functions, replay/bench/<drv>.cpp the measurement;
# bench only: replay/bench/<bench>.cpp holds both.
P = {
    "liquibook": dict(drv="liquibook", paths=["src/book", "src/simple"], inc=["src"], srcs=["src/simple/simple_order.cpp"]),
    "quill": dict(drv="quill", paths=["include"], inc=["include"], srcs=[], cores=2),
    "concurrentqueue": dict(drv="concurrentqueue", paths=["concurrentqueue.h", "blockingconcurrentqueue.h", "lightweightsemaphore.h"],
                            inc=["."], srcs=[], flags=["-DMOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED"]),
    "atomic_queue": dict(drv="atomic_queue", paths=["include"], inc=["include"], srcs=[]),
    "SPSCQueue": dict(drv="spscqueue", paths=["include"], inc=["include"], srcs=[]),
    "MPMCQueue": dict(drv="mpmcqueue", paths=["include"], inc=["include"], srcs=[]),
    "hffix": dict(drv="hffix", paths=["include"], inc=["include"], srcs=[]),
    "spdlog": dict(bench="spdlog", paths=["include"], inc=["include"], srcs=[]),
    "fmt": dict(bench="fmt", paths=["include"], inc=["include"], srcs=[]),
    "simdjson": dict(bench="simdjson", paths=["singleheader/simdjson.h", "singleheader/simdjson.cpp"],
                     inc=["singleheader"], srcs=["singleheader/simdjson.cpp"]),
    "unordered_dense": dict(bench="unordered_dense", paths=["include"], inc=["include"], srcs=[]),
    "robin-map": dict(bench="robin-map", paths=["include"], inc=["include"], srcs=[]),
    "parallel-hashmap": dict(bench="parallel-hashmap", paths=["parallel_hashmap"], inc=["."], srcs=[]),
    "robin-hood-hashing": dict(bench="robin-hood-hashing", paths=["src/include"], inc=["src/include"], srcs=[]),
    "fast_float": dict(bench="fast_float", paths=["include"], inc=["include"], srcs=[]),
    "xxHash": dict(bench="xxHash", paths=["xxhash.h", "xxh3.h"], inc=["."], srcs=[]),
    "readerwriterqueue": dict(bench="readerwriterqueue", paths=["readerwriterqueue.h", "atomicops.h"], inc=["."], srcs=[]),
    "eigen": dict(bench="eigen", paths=["Eigen"], inc=["."], srcs=[]),
}
CORES = [int(c) for c in os.environ.get("REPLAY_CORES", "2,4,6,8").split(",")]
lock = threading.Lock(); free_cores = list(CORES)

def commits(proj):
    out = subprocess.run(["git", "-C", str(TOP / "src" / proj), "log", "--first-parent", "--reverse", "--format=%H %ad %s",
                          "--date=short", "--", *P[proj]["paths"]], capture_output=True, text=True, check=True).stdout
    return [l.split(" ", 2) for l in out.splitlines()]

def outdir(proj, cc):
    return TOP / "out" / (cc + TAG) / proj

def build_cmd(cfg, cc, tree, exe, std="-std=c++17", extra=()):
    bench = HERE / "bench" / f"{cfg.get('drv') or cfg['bench']}.cpp"
    drv = [f"-I{HERE / 'drivers' / cfg['drv']}"] if "drv" in cfg else []
    return [CXX[cc], std, OPT, "-DNDEBUG", "-pthread", "-march=x86-64-v3", "-w", *extra,
            *[f"-I{tree / i}" for i in cfg["inc"]], *drv, *cfg.get("flags", []),
            str(bench), *[str(tree / s) for s in cfg["srcs"]], "-o", str(exe), "-ldl"]

def build(proj, cc, h, tree, exe, extra=()):
    """Build at the commit; C++17 first, then C++20 (newer versions of some projects need it)."""
    subprocess.run(f"git -C {TOP / 'src' / proj} archive {h} | tar -x -C {tree}", shell=True, check=True)
    b = subprocess.run(build_cmd(P[proj], cc, tree, exe, extra=extra), capture_output=True, text=True)
    if b.returncode:
        b2 = subprocess.run(build_cmd(P[proj], cc, tree, exe, "-std=c++20", extra), capture_output=True, text=True)
        if b2.returncode == 0: b = b2
    return b

def one(proj, cc, h):
    out = outdir(proj, cc); res = out / f"{h}.jsonl"; fail = out / f"{h}.fail"
    if res.exists() or fail.exists(): return h, "cached"
    with tempfile.TemporaryDirectory() as t:
        t = Path(t); exe = t / "_bench_exe"
        b = build(proj, cc, h, t, exe)
        if b.returncode:
            fail.write_text("BUILD\n" + b.stderr[:4000]); return h, "build-fail"
        n = P[proj].get("cores", 1)
        with lock: cs = [free_cores.pop(0) for _ in range(n)]
        try:
            r = subprocess.run(["taskset", "-c", ",".join(map(str, cs)), str(exe)], capture_output=True, text=True, timeout=600)
        finally:
            with lock: free_cores[:0] = cs
        lines = [l for l in r.stdout.splitlines() if l.startswith('{"op"')]
        if r.returncode or not lines:
            fail.write_text(f"RUN rc={r.returncode}\n" + r.stderr[-3000:]); return h, "run-fail"
        res.write_text("\n".join(lines) + "\n")
    return h, "ok"

def counts_of(out, h):
    f = out / f"{h}.jsonl"
    if not f.exists(): return None
    return {d["op"]: tuple(round(d[k], 2) for k in ("alloc", "free", "lock", "first_alloc", "first_lock") if k in d)
            for d in map(json.loads, f.read_text().splitlines())}

def differ(x, y):
    for op in set(x) & set(y):
        n = min(len(x[op]), len(y[op]))
        if x[op][:n] != y[op][:n]: return True
    return False

def refine(proj, cc, cs, j):
    """Bisect every interval between measured commits whose counts differ, until the change is pinned to
    one commit or to a run of commits that do not build. Counts are exact, so bisection is sound as long
    as a count changes once inside an interval; an interval where it changes and changes back is only
    found if a sample falls inside it."""
    out = outdir(proj, cc)
    while True:
        done = [i for i, c in enumerate(cs) if (out / f"{c[0]}.jsonl").exists() or (out / f"{c[0]}.fail").exists()]
        ok = [i for i in done if (out / f"{cs[i][0]}.jsonl").exists()]
        todo = []
        for a, b in zip(ok, ok[1:]):
            if b - a > 1 and differ(counts_of(out, cs[a][0]), counts_of(out, cs[b][0])):
                untried = [i for i in range(a + 1, b) if i not in done]
                if untried: todo.append(untried[len(untried) // 2])
        if not todo: break
        with concurrent.futures.ThreadPoolExecutor(j) as ex:
            for h, st in ex.map(lambda i: one(proj, cc, cs[i][0]), todo):
                print("refine", h[:8], st, flush=True)

def main():
    proj = sys.argv[1]
    cc = sys.argv[sys.argv.index("--cc") + 1] if "--cc" in sys.argv else "gcc"
    j = int(sys.argv[sys.argv.index("-j") + 1]) if "-j" in sys.argv else len(CORES)
    out = outdir(proj, cc); out.mkdir(parents=True, exist_ok=True)
    cs = commits(proj)
    json.dump(cs, open(out / "order.json", "w"))
    if len(CORES) < P[proj].get("cores", 1):
        sys.exit(f"{proj}: the benchmark runs {P[proj]['cores']} threads; give as many cores in REPLAY_CORES")
    j = max(1, min(j, len(CORES) // P[proj].get("cores", 1)))
    if "--refine" in sys.argv:
        return refine(proj, cc, cs, j)
    if "--stride" in sys.argv:
        k = int(sys.argv[sys.argv.index("--stride") + 1])
        cs = cs[::k] + ([cs[-1]] if (len(cs) - 1) % k else [])
    if "--commits" in sys.argv:
        want = sys.argv[sys.argv.index("--commits") + 1].split(",")
        cs = [c for c in cs if any(c[0].startswith(w) for w in want)]
    with concurrent.futures.ThreadPoolExecutor(j) as ex:
        for h, st in ex.map(lambda c: one(proj, cc, c[0]), cs):
            print(h[:8], st, flush=True)

if __name__ == "__main__":
    main()
