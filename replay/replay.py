#!/usr/bin/env python3
"""Replay a library's history with exact per-operation counts.

At first-parent commits that changed the library, build replay/workloads/<workload>.cpp against that commit
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
import re, concurrent.futures, json, os, subprocess, sys, tempfile, threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOP = HERE.parent
CXX = {"gcc": os.environ.get("CXX_GCC", "g++"), "gcc15": os.environ.get("CXX_GCC15", "g++-15"),
       "clang": os.environ.get("CXX_CLANG", "clang++")}
OPT = os.environ.get("REPLAY_OPT", "-O2")
TAG = os.environ.get("REPLAY_TAG", "" if OPT == "-O2" else OPT)
# --metric instr: instructions per call, counted by valgrind's callgrind (exact and independent of load)
METRIC = sys.argv[sys.argv.index("--metric") + 1] if "--metric" in sys.argv else "counts"
VALGRIND = os.environ.get("VALGRIND", "valgrind")
VALGRIND_INC = os.environ.get("VALGRIND_INC", "")   # directory holding valgrind/callgrind.h

# drv: replay/drivers/<drv>/main.cpp holds the entry functions, replay/workloads/<drv>.cpp the workload that
# calls them; workload only: replay/workloads/<workload>.cpp holds both.
P = {
    "liquibook": dict(drv="liquibook", paths=["src/book", "src/simple"], inc=["src"], srcs=["src/simple/simple_order.cpp"]),
    "quill": dict(drv="quill", paths=["include"], inc=["include"], srcs=[], cores=2),
    "concurrentqueue": dict(drv="concurrentqueue", paths=["concurrentqueue.h", "blockingconcurrentqueue.h", "lightweightsemaphore.h"],
                            inc=["."], srcs=[], flags=["-DMOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED"]),
    "atomic_queue": dict(drv="atomic_queue", paths=["include"], inc=["include"], srcs=[]),
    "SPSCQueue": dict(drv="spscqueue", paths=["include"], inc=["include"], srcs=[]),
    "MPMCQueue": dict(drv="mpmcqueue", paths=["include"], inc=["include"], srcs=[]),
    "hffix": dict(drv="hffix", paths=["include"], inc=["include"], srcs=[]),
    "spdlog": dict(workload="spdlog", paths=["include"], inc=["include"], srcs=[]),
    "fmt": dict(workload="fmt", paths=["include"], inc=["include"], srcs=[]),
    "simdjson": dict(workload="simdjson", paths=["singleheader/simdjson.h", "singleheader/simdjson.cpp"],
                     inc=["singleheader"], srcs=["singleheader/simdjson.cpp"]),
    "unordered_dense": dict(workload="unordered_dense", paths=["include"], inc=["include"], srcs=[]),
    "robin-map": dict(workload="robin-map", paths=["include"], inc=["include"], srcs=[]),
    "parallel-hashmap": dict(workload="parallel-hashmap", paths=["parallel_hashmap"], inc=["."], srcs=[]),
    "robin-hood-hashing": dict(workload="robin-hood-hashing", paths=["src/include"], inc=["src/include"], srcs=[]),
    "fast_float": dict(workload="fast_float", paths=["include"], inc=["include"], srcs=[]),
    "xxHash": dict(workload="xxHash", paths=["xxhash.h", "xxh3.h"], inc=["."], srcs=[]),
    "readerwriterqueue": dict(workload="readerwriterqueue", paths=["readerwriterqueue.h", "atomicops.h"], inc=["."], srcs=[]),
    "eigen": dict(workload="eigen", paths=["Eigen"], inc=["."], srcs=[]),
    "json": dict(workload="json", paths=["single_include"], inc=["single_include"], srcs=[]),
    "taskflow": dict(workload="taskflow", paths=["taskflow"], inc=["."], srcs=[], cores=2),
    "pdqsort": dict(workload="pdqsort", paths=["pdqsort.h"], inc=["."], srcs=[]),
    "lz4": dict(workload="lz4", paths=["lib/lz4.c", "lib/lz4.h"], inc=["lib"], srcs=[]),
    "dragonbox": dict(workload="dragonbox", paths=["include"], inc=["include"], srcs=[]),
    "libdivide": dict(workload="libdivide", paths=["libdivide.h"], inc=["."], srcs=[]),
    "date": dict(workload="date", paths=["include/date/date.h"], inc=["include"], srcs=[]),
    # A random order flow on the same driver, to reach the paths a fixed workload does not take
    # (src/liquibook-explore is a link to src/liquibook; fetch.sh makes it).
    "liquibook-explore": dict(drv="liquibook", workload="liquibook_explore", paths=["src/book", "src/simple"], inc=["src"],
                              srcs=["src/simple/simple_order.cpp"]),
    "concurrentqueue-explore": dict(drv="concurrentqueue", workload="concurrentqueue_explore",
                                    paths=["concurrentqueue.h", "blockingconcurrentqueue.h", "lightweightsemaphore.h"],
                                    inc=["."], srcs=[], flags=["-DMOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED"]),
    "fmtlog": dict(workload="fmtlog", paths=["fmtlog.h", "fmtlog-inl.h", "fmt"], inc=[".", "fmt/include"], srcs=[],
                   submodules={"fmt": "fmt"}, flags=["-DFMTLOG_HEADER_ONLY", "-DFMT_HEADER_ONLY"], cores=2),
    "bitsery": dict(workload="bitsery", paths=["include"], inc=["include"], srcs=[]),
    "cereal": dict(workload="cereal", paths=["include"], inc=["include"], srcs=[]),
    "xsimd": dict(workload="xsimd", paths=["include"], inc=["include"], srcs=[]),
    "yyjson": dict(workload="yyjson", paths=["src"], inc=["src"], srcs=[]),
    "glaze": dict(workload="glaze", paths=["include"], inc=["include"], srcs=[], std="c++23"),
    "rapidjson": dict(workload="rapidjson", paths=["include"], inc=["include"], srcs=[]),
    "flatbuffers": dict(workload="flatbuffers", paths=["include/flatbuffers"], inc=["include"], srcs=[]),
    "msgpack-c": dict(workload="msgpack-c", paths=["include"], inc=["include"], srcs=[], flags=["-DMSGPACK_NO_BOOST"]),
    "gtl": dict(workload="gtl", paths=["include"], inc=["include"], srcs=[]),
    "emhash": dict(workload="emhash", paths=["include", "hash_table8.hpp"], inc=["include/emhash", "."], srcs=[]),
    "abseil": dict(workload="abseil", paths=["absl/container", "absl/hash"], inc=["."],
                   # each compiled where it exists at the commit; the hashtablez sampling functions are left
                   # unresolved (see replay/workloads/abseil.cpp), and ABSL_CONSUME_DLL turns sampling off in
                   # the versions that enabled it by default
                   srcs=[], srcs_if_present=["absl/container/internal/raw_hash_set.cc", "absl/hash/internal/hash.cc",
                         "absl/hash/internal/city.cc", "absl/hash/internal/low_level_hash.cc",
                         "absl/base/internal/raw_logging.cc"],
                   # from c349faaf (2026-05-13) to d851fdd7 (2026-06-09) raw_hash_set.h includes <bmi2intrin.h>
                   # directly, which GCC rejects; including the header it asks for first, in every
                   # translation unit, lets those commits build
                   flags=["-DABSL_CONSUME_DLL", "-include", "x86gprintrin.h"],
                   unresolved_ok=["ForcedTrySample", "SampleSlow", "UnsampleSlow", "RecordInsertSlow",
                                  "RecordRehashSlow", "RecordStorageChangedSlow", "RecordEraseSlow",
                                  "RecordReservationSlow", "RecordClearedReservationSlow", "SampleForTesting"]),
    "highway": dict(workload="highway", paths=["hwy"], inc=["."], srcs=[]),
}
CORES = [int(c) for c in os.environ.get("REPLAY_CORES", "2,4,6,8").split(",")]
lock = threading.Lock(); free_cores = list(CORES)

def commits(proj):
    out = subprocess.run(["git", "-C", str(TOP / "src" / proj), "log", "--first-parent", "--reverse", "--format=%H %ad %s",
                          "--date=short", "--", *P[proj]["paths"]], capture_output=True, text=True, check=True).stdout
    return [l.split(" ", 2) for l in out.splitlines()]

def outdir(proj, cc):
    return TOP / "out" / (cc + TAG + ("-instr" if METRIC == "instr" else "")) / proj

def fingerprint(proj, cc):
    """A hash of everything besides the library commit that a cached result depends on: the workload, its
    driver, the shared headers, the project's configuration, the compiler and the options."""
    import hashlib
    cfg = P[proj]; h = hashlib.sha256()
    files = [HERE / "workloads" / f"{cfg.get('workload') or cfg['drv']}.cpp", HERE / "workload.h", HERE / "counts.h"]
    files += sorted((HERE / "workloads").glob("*.h"))
    if "drv" in cfg: files += sorted(p for p in (HERE / "drivers" / cfg["drv"]).rglob("*") if p.is_file())
    files += [HERE / "drivers" / "hp_entry.h"]
    for f in files:   # lines that are only a // comment do not count: editing a comment keeps the results
        if f.exists():
            h.update(f.name.encode())
            h.update(b"\n".join(l for l in f.read_bytes().splitlines() if not l.lstrip().startswith(b"//")))
    h.update(repr(sorted(cfg.items())).encode()); h.update(f"{CXX[cc]} {OPT} {METRIC}".encode())
    return h.hexdigest()[:16]

def check_cache(proj, cc):
    """Set cached results aside when the workload, driver or configuration changed since they were made,
    so a changed workload is never compared with results of the old one."""
    out = outdir(proj, cc); fp_file = out / "fingerprint"; now = fingerprint(proj, cc)
    old = fp_file.read_text().strip() if fp_file.exists() else None
    cached = list(out.glob("*.jsonl")) + list(out.glob("*.fail"))
    if old is not None and old != now and cached:
        stale = out / f"stale-{old}"; stale.mkdir(exist_ok=True)
        for f in cached: f.rename(stale / f.name)
        print(f"{proj}: the workload or its configuration changed; {len(cached)} cached results moved to {stale}", flush=True)
    fp_file.write_text(now + "\n")

def build_cmd(cfg, cc, tree, exe, std="-std=c++17", extra=()):
    workload = HERE / "workloads" / f"{cfg.get('workload') or cfg['drv']}.cpp"
    drv = [f"-I{HERE / 'drivers' / cfg['drv']}"] if "drv" in cfg else []
    return [CXX[cc], std, OPT, "-DNDEBUG", "-pthread", "-march=x86-64-v3", "-w", *extra,
            *[f"-I{tree / i}" for i in cfg["inc"]], *drv, *cfg.get("flags", []),
            str(workload), *[str(tree / s) for s in cfg["srcs"]],
            *[str(tree / s) for s in cfg.get("srcs_if_present", []) if (tree / s).exists()], "-o", str(exe), "-ldl"]

def build(proj, cc, h, tree, exe, extra=()):
    """Build at the commit; C++17 first, then C++20 (newer versions of some projects need it), unless the
    project sets its own standard ("std" in P)."""
    subprocess.run(f"git -C {TOP / 'src' / proj} archive {h} | tar -x -C {tree}", shell=True, check=True)
    # git submodules: the commit each one is pinned to at h, taken from a local clone in src/
    for path, clone in P[proj].get("submodules", {}).items():
        ls = subprocess.run(["git", "-C", str(TOP / "src" / proj), "ls-tree", h, path], capture_output=True, text=True).stdout.split()
        if len(ls) >= 3 and ls[1] == "commit":
            (tree / path).mkdir(parents=True, exist_ok=True)
            subprocess.run(f"git -C {TOP / 'src' / clone} archive {ls[2]} | tar -x -C {tree / path}", shell=True)
    std0 = "-std=" + P[proj].get("std", "c++17")   # a project may need a newer standard throughout
    # functions named in "unresolved_ok" may stay undefined (paths the workload never reaches); any other
    # undefined symbol, and every undefined variable, fails the build: a variable resolved to address 0
    # would be read and written without a crash
    ok = P[proj].get("unresolved_ok")
    link = ("-Wl,--warn-unresolved-symbols",) if ok else ()
    def attempt(std):
        b = subprocess.run(build_cmd(P[proj], cc, tree, exe, std, (*extra, *link)), capture_output=True, text=True)
        if ok and b.returncode == 0:
            bad = sorted({sym for sym in re.findall(r"undefined reference to `([^']+)'", b.stderr)
                          if "(" not in sym or not any(re.search(r"::" + n + r"\(", sym) for n in ok)})
            if bad: return subprocess.CompletedProcess(b.args, 1, b.stdout, "unresolved symbols not allowed:\n" + "\n".join(bad))
        return b
    b = attempt(std0)
    if b.returncode and std0 == "-std=c++17":
        b2 = attempt("-std=c++20")
        if b2.returncode == 0: b = b2
    return b

def one(proj, cc, h):
    out = outdir(proj, cc); res = out / f"{h}.jsonl"; fail = out / f"{h}.fail"
    if res.exists() or fail.exists(): return h, "cached"
    with tempfile.TemporaryDirectory() as t:
        t = Path(t); exe = t / "_workload_exe"
        extra = ("-DREPLAY_INSTR", *([f"-I{VALGRIND_INC}"] if VALGRIND_INC else [])) if METRIC == "instr" else ()
        b = build(proj, cc, h, t, exe, extra=extra)
        if b.returncode:
            fail.write_text("BUILD\n" + b.stderr[:4000]); return h, "build-fail"
        n = P[proj].get("cores", 1)
        with lock: cs = [free_cores.pop(0) for _ in range(n)]
        try:
            run = [str(exe)] if METRIC != "instr" else [VALGRIND, "--tool=callgrind", f"--callgrind-out-file={t}/cg.out",
                                                         "--dump-instr=no", "--compress-strings=no", "--compress-pos=no", str(exe)]
            r = subprocess.run(["taskset", "-c", ",".join(map(str, cs)), *run], capture_output=True, text=True,
                               timeout=600 if METRIC != "instr" else 3600)
        finally:
            with lock: free_cores[:0] = cs
        lines = [l for l in r.stdout.splitlines() if l.startswith('{"op"')]
        if METRIC == "instr" and not r.returncode:
            lines = instr_lines(t, r.stdout)
        if r.returncode or not lines:
            fail.write_text(f"RUN rc={r.returncode}\n" + r.stderr[-3000:]); return h, "run-fail"
        res.write_text("\n".join(lines) + "\n")
    return h, "ok"

def instr_lines(t, stdout):
    """One JSON line per operation with instructions per call, from callgrind's per-operation dumps."""
    n = {d["instr_op"]: d["instr_n"] for d in (json.loads(l) for l in stdout.splitlines() if l.startswith('{"instr_op"'))}
    out = []
    for f in sorted(Path(t).glob("cg.out.*")):
        name = ir = None
        for line in f.read_text(errors="replace").splitlines():
            if line.startswith("desc: Trigger: Client Request: "): name = line[len("desc: Trigger: Client Request: "):].strip()
            elif line.startswith(("summary:", "totals:")) and ir is None: ir = int(line.split()[1])
        if name in n and ir is not None:
            out.append(json.dumps({"op": name, "instr": round(ir / n[name], 2)}))
    return out

INSTR_PCT = float(os.environ.get("REPLAY_INSTR_PCT", "3"))   # instruction changes below this are not bisected

def compared_fields(proj):
    """The count fields compared between commits. With another thread in the workload (cores > 1), the
    frees counted on the calling thread depend on which thread drops the last reference first, so they
    vary between runs of the same build; allocations and locks on the calling thread do not."""
    f = ("alloc", "free", "lock", "first_alloc", "first_lock")
    return tuple(k for k in f if k != "free") if P[proj].get("cores", 1) > 1 else f

def counts_of(out, h, fields=("alloc", "free", "lock", "first_alloc", "first_lock")):
    f = out / f"{h}.jsonl"
    if not f.exists(): return None
    return {d["op"]: (("instr", d["instr"]),) if "instr" in d else
            tuple(round(d[k], 2) for k in fields if k in d)
            for d in map(json.loads, f.read_text().splitlines())}

def differ(x, y):
    for op in set(x) & set(y):
        a, b = x[op], y[op]
        is_instr = lambda v: bool(v) and isinstance(v[0], tuple) and v[0][0] == "instr"
        if is_instr(a) and is_instr(b):
            if abs(a[0][1] - b[0][1]) * 100 >= INSTR_PCT * max(a[0][1], 1): return True
            continue
        n = min(len(a), len(b))
        if a[:n] != b[:n]: return True
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
            fl = compared_fields(proj)
            if b - a > 1 and differ(counts_of(out, cs[a][0], fl), counts_of(out, cs[b][0], fl)):
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
    check_cache(proj, cc)
    cs = commits(proj)
    json.dump(cs, open(out / "order.json", "w"))
    if len(CORES) < P[proj].get("cores", 1):
        sys.exit(f"{proj}: the workload runs {P[proj]['cores']} threads; give as many cores in REPLAY_CORES")
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
