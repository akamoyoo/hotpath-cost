#!/usr/bin/env python3
"""Count what a library's own Google Benchmark benchmarks do per iteration, at any commit.

  bench.py LIB COMMIT [COMMIT ...] [--filter REGEX] [--instr]    one row per benchmark and commit
  bench.py LIB --history [--stride N] [--filter REGEX] [-j J]    every library commit, into out/gcc-bench/LIB/
  then: diff.py LIB --tag -bench                                 which commits changed a count

The library's benchmark program is built from the commit with its own CMake build (BENCH below), against
one installed Google Benchmark (GBENCH_DIR). No workload is written and no source is changed: each
benchmark is run twice under LD_PRELOAD=replay/preload/counts_preload.so, for N and 3N iterations
(--benchmark_min_time=<N>x), and the difference of the process-wide counts divided by the difference of
the iterations is what one iteration does; set-up and tear-down cancel. With --instr the same is done
for instructions, under callgrind. Times are Google Benchmark's own, per iteration, and are not exact.

Counts are process-wide: a benchmark whose logger or executor runs another thread includes that
thread's allocations and locks.
"""
import json, os, re, subprocess, sys, tempfile, concurrent.futures
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import replay as rp

GBENCH_DIR = os.environ.get("GBENCH_DIR", "")   # the prefix Google Benchmark was installed into
GBENCH_SRC = os.environ.get("GBENCH_SRC", "")   # its source tree, for builds that add it with FetchContent
VALGRIND = os.environ.get("VALGRIND", "valgrind")
RUN_SECONDS = 0.05   # each of the two runs of a benchmark lasts about this long; the second runs 3x the first

# How to build each library's benchmark program:
#   cmake      CMake options; {lib} is the library's tree at the commit measured, {gbench_src} GBENCH_SRC,
#              {data:NAME} the directory of a data set in "data"
#   project    the CMake project's directory within the tree (default: its root)
#   bench_tree the commit whose benchmark sources are used for every commit measured (the library comes
#              from the commit measured, through an option in "cmake"); default: the commit measured
#   bench_paths the paths taken from bench_tree (default: project)
#   data       data sets to download once: {name: zip URL}
#   target, binary, paths: the target to build, the binary's path in the build tree, the paths whose
#              commits matter
BENCH = {
    "spdlog": dict(cmake=["-DSPDLOG_BUILD_BENCH=ON", "-DSPDLOG_BUILD_EXAMPLE=OFF", "-DSPDLOG_BUILD_TESTS=OFF"],
                   target="latency", binary="bench/latency", paths=["include", "src", "bench"]),
    # nlohmann/json's benchmarks are built to compare versions: the benchmark sources of the latest commit,
    # the header of the commit measured (JSON_BENCHMARK_INCLUDE_DIR), the test data and Google Benchmark
    # from local copies instead of downloads
    "json": dict(project="tests/benchmarks", bench_tree="HEAD", bench_paths=["tests/benchmarks", "cmake"],
                 cmake=["-DJSON_BENCHMARK_INCLUDE_DIR={lib}/single_include", "-DJSON_TestDataDirectory={data:json_test_data}",
                        "-DFETCHCONTENT_SOURCE_DIR_BENCHMARK={gbench_src}"],
                 data={"json_test_data": "https://github.com/nlohmann/json_test_data/archive/refs/tags/v3.2.0.zip"},
                 target="json_benchmarks", binary="json_benchmarks", paths=["single_include"]),
}

def data_dir(name, url):
    """A data set, downloaded and unpacked once into out/bench-data/NAME."""
    d = rp.TOP / "out" / "bench-data" / name
    if not d.exists():
        d.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as t:
            subprocess.run(["curl", "-sSL", "-o", f"{t}/d.zip", url], check=True)
            subprocess.run(["unzip", "-q", f"{t}/d.zip", "-d", f"{t}/x"], check=True)
            inner = [p for p in Path(t, "x").iterdir()]
            (inner[0] if len(inner) == 1 and inner[0].is_dir() else Path(t, "x")).rename(d)
    return d

def preload():
    so = HERE / "preload" / "counts_preload.so"
    src = HERE / "preload" / "counts_preload.c"
    if not so.exists() or so.stat().st_mtime < src.stat().st_mtime:
        subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-o", str(so), str(src), "-ldl"], check=True)
    return so

def build(lib, commit, tree):
    """Build the benchmark program at the commit; returns its path, or an error text."""
    cfg = BENCH[lib]
    lib_tree = tree / "_lib" if cfg.get("bench_tree") else tree
    lib_tree.mkdir(exist_ok=True)
    subprocess.run(f"git -C {rp.TOP / 'src' / lib} archive {commit} | tar -x -C {lib_tree}", shell=True, check=True)
    if cfg.get("bench_tree"):
        paths = " ".join(cfg.get("bench_paths", [cfg.get("project", ".")]))
        subprocess.run(f"git -C {rp.TOP / 'src' / lib} archive {cfg['bench_tree']} {paths} | tar -x -C {tree}", shell=True, check=True)
    def fill(a):
        a = a.replace("{lib}", str(lib_tree)).replace("{gbench_src}", GBENCH_SRC)
        return re.sub(r"\{data:(\w+)\}", lambda m: str(data_dir(m.group(1), cfg["data"][m.group(1)])), a)
    args = ["cmake", "-S", str(tree / cfg.get("project", ".")), "-B", str(tree / "_build"), "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_CXX_COMPILER={rp.CXX['gcc']}", *map(fill, cfg["cmake"])]
    if GBENCH_DIR:   # the installed Google Benchmark, and no download of another (older versions do not
                     # take a fixed iteration count); a build that cannot find it fails instead
        args += [f"-DCMAKE_PREFIX_PATH={GBENCH_DIR}", f"-Dbenchmark_DIR={Path(GBENCH_DIR) / 'lib' / 'cmake' / 'benchmark'}",
                 "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"]
    for cmd in (args, ["cmake", "--build", str(tree / "_build"), "-j4", "--target", cfg["target"]]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: return None, (r.stdout + r.stderr)[-3000:]
    exe = tree / "_build" / cfg["binary"]
    return (exe, "") if exe.exists() else (None, f"no binary at {cfg['binary']}")

def std_regex_escape(name):
    """Escape a benchmark name for Google Benchmark's std::regex (ECMAScript): only the metacharacters,
    since an escaped '-' or ' ', as Python's re.escape writes them, is an error there."""
    return re.sub(r"([.^$|()\[\]{}*+?\\])", r"\\\1", name)

def run_once(exe, name, n, core, instr=False):
    """One run of one benchmark for n iterations: (process-wide counts, iterations, ns per iteration)."""
    with tempfile.TemporaryDirectory() as t:
        out = Path(t) / "counts"
        env = dict(os.environ, LD_PRELOAD=str(preload()), COUNTS_OUT=str(out))
        cmd = [str(exe), f"--benchmark_filter=^{std_regex_escape(name)}$", f"--benchmark_min_time={n}x",
               "--benchmark_repetitions=1", "--benchmark_format=json"]
        if instr:
            env.pop("LD_PRELOAD")
            cmd = [VALGRIND, "--tool=callgrind", f"--callgrind-out-file={t}/cg.out", *cmd]
        r = subprocess.run(["taskset", "-c", str(core), *cmd], capture_output=True, text=True, env=env, cwd=t,
                           timeout=1800)
        try:
            res = next(b for b in json.loads(r.stdout)["benchmarks"] if b.get("run_type", "iteration") == "iteration")
        except (ValueError, StopIteration, KeyError):
            return None
        if res["iterations"] != n:   # the iteration count was not ours: trial runs were counted too
            return None
        iters = res["iterations"] * max(1, int(res.get("threads", 1)))
        ns = res["real_time"] * {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}[res.get("time_unit", "ns")]
        if instr:
            text = (Path(t) / "cg.out").read_text(errors="replace")
            m = re.search(r"^(?:summary|totals): (\d+)", text, re.M)
            return ({"instr": int(m.group(1))} if m else None), iters, ns
        if not out.exists(): return None
        f = out.read_text().split()
        return {"alloc": int(f[1]), "free": int(f[3]), "lock": int(f[5])}, iters, ns

def measure(exe, filt, core, instr=False, iters=None):
    """Per-iteration counts of every benchmark (matching filt): {name: record}. iters ({name: N}) holds the
    iteration counts to use, and receives those chosen for benchmarks it lacks: the fixed costs of a run
    (Google Benchmark's own output, for one) depend slightly on N, so commits are compared at the same N."""
    iters = {} if iters is None else iters
    with tempfile.TemporaryDirectory() as t:   # programs may write files where they run (spdlog: its logs)
        names = subprocess.run([str(exe), "--benchmark_list_tests"], capture_output=True, text=True, cwd=t).stdout.split("\n")
    out = {}
    for name in (n for n in names if n and (not filt or re.search(filt, n))):
        if name not in iters:
            probe = run_once(exe, name, 1, core)   # how long one iteration takes, to choose the iteration counts
            if not probe: continue
            # at least 100: the two runs differ by 200 iterations or more, so that a fixed cost that moves by
            # one allocation moves a per-iteration count by 0.005 at most
            iters[name] = max(100, min(20000, int(RUN_SECONDS / max(probe[2], 1) * 1e9)))
        n1 = iters[name]
        a, b = run_once(exe, name, n1, core, instr), run_once(exe, name, 3 * n1, core, instr)
        if not a or not b or not a[0] or not b[0] or b[1] == a[1]:
            continue
        d = b[1] - a[1]
        rec = {"op": name, **{k: round((b[0][k] - a[0][k]) / d, 4) for k in a[0]}, "ns": round(b[2], 1)}
        if not instr: rec.update(first_alloc=0, first_lock=0)
        if re.search(r"/threads:(\d+)", name) and int(re.search(r"/threads:(\d+)", name).group(1)) > 1:
            rec["exact"] = False   # several threads: what each iteration does depends on scheduling
        out[name] = rec
    return out

def one(lib, commit, filt, core, instr, outdir=None, iters=None):
    with tempfile.TemporaryDirectory() as t:
        exe, err = build(lib, commit, Path(t))
        if exe is None:
            if outdir: (outdir / f"{commit}.fail").write_text("BUILD\n" + err)
            return commit, None, err
        res = measure(exe, filt, core, instr, iters)
        if outdir:
            if res: (outdir / f"{commit}.jsonl").write_text("".join(json.dumps(r) + "\n" for r in res.values()))
            else: (outdir / f"{commit}.fail").write_text("RUN no benchmark result (not a Google Benchmark program at this commit, "
                                                           "or every iteration count was overridden)\n")
        return commit, res, ""

def load_iters(outdir):
    f = outdir / "iterations.json"
    return json.load(open(f)) if f.exists() else {}

def save_iters(outdir, iters):
    json.dump(iters, open(outdir / "iterations.json", "w"), indent=1)

def on_cores(fn, items):
    """fn(item, core) for every item, in parallel, each holding one core of REPLAY_CORES while it runs."""
    import queue
    cores = queue.Queue()
    for c in rp.CORES: cores.put(c)
    def job(item):
        core = cores.get()
        try: return fn(item, core)
        finally: cores.put(core)
    with concurrent.futures.ThreadPoolExecutor(len(rp.CORES)) as ex:
        yield from ex.map(job, items)

def exact_counts(outdir, c, instr):
    """{op: (alloc, lock)} or {op: instr} of one measured commit, leaving out benchmarks marked inexact."""
    f = outdir / f"{c}.jsonl"
    if not f.exists(): return None
    keys = ("instr",) if instr else ("alloc", "lock")
    return {r["op"]: tuple(round(r[k], 2) for k in keys) for r in map(json.loads, f.read_text().splitlines())
            if r.get("exact", True)}

def refine(lib, cs, outdir, filt, instr):
    """Bisect every interval between measured commits whose exact counts differ, until each change is at
    one commit or between commits that do not build. Instructions differ when they move by 3 % or more."""
    def differ(a, b):
        for op in set(a) & set(b):
            x, y = a[op], b[op]
            if instr and abs(x[0] - y[0]) * 100 >= 3 * max(x[0], 1): return True
            if not instr and any(abs(p - q) >= 0.01 for p, q in zip(x, y)): return True
        return False
    while True:
        done = [i for i, c in enumerate(cs) if (outdir / f"{c[0]}.jsonl").exists() or (outdir / f"{c[0]}.fail").exists()]
        ok = [i for i in done if (outdir / f"{cs[i][0]}.jsonl").exists()]
        todo = []
        for a, b in zip(ok, ok[1:]):
            if b - a > 1 and differ(exact_counts(outdir, cs[a][0], instr), exact_counts(outdir, cs[b][0], instr)):
                untried = [i for i in range(a + 1, b) if i not in done]
                if untried: todo.append(untried[len(untried) // 2])
        if not todo: return
        iters = load_iters(outdir)
        for c, res, err in on_cores(lambda c, core: one(lib, c, filt, core, instr, outdir, iters), [cs[i][0] for i in todo]):
            save_iters(outdir, iters)
            print("refine", c[:8], "ok" if res else "fail", flush=True)

def main():
    valued = {"--filter", "--stride", "-j"}   # options that take a value: their value is not a commit
    argv = sys.argv[1:]
    args = [a for i, a in enumerate(argv) if not a.startswith("-") and not (i and argv[i - 1] in valued)]
    opt = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    lib = args[0]; filt = opt("--filter"); instr = "--instr" in sys.argv
    if lib not in BENCH: sys.exit(f"{lib}: no benchmark build configured in BENCH")
    if any("{gbench_src}" in a for a in BENCH[lib]["cmake"]) and not (Path(GBENCH_SRC) / "CMakeLists.txt").exists():
        sys.exit(f"{lib}: set GBENCH_SRC to a Google Benchmark source tree (1.8 or later)")
    if GBENCH_DIR and not (Path(GBENCH_DIR) / "lib" / "cmake" / "benchmark").is_dir():
        sys.exit(f"GBENCH_DIR={GBENCH_DIR}: no lib/cmake/benchmark there (the prefix Google Benchmark was installed into)")
    src = rp.TOP / "src" / lib
    if "--history" in sys.argv:
        log = subprocess.run(["git", "-C", str(src), "log", "--first-parent", "--reverse", "--format=%H %ad %s",
                              "--date=short", "--", *BENCH[lib]["paths"]], capture_output=True, text=True, check=True).stdout
        cs = [l.split(" ", 2) for l in log.splitlines()]
        outdir = rp.TOP / "out" / ("gcc-bench" + ("-instr" if instr else "")) / lib
        outdir.mkdir(parents=True, exist_ok=True)
        json.dump(cs, open(outdir / "order.json", "w"))
        k = int(opt("--stride", "1"))
        todo = [c[0] for c in cs[::k] + ([cs[-1]] if (len(cs) - 1) % k else [])
                if not (outdir / f"{c[0]}.jsonl").exists() and not (outdir / f"{c[0]}.fail").exists()]
        if "--refine" in sys.argv:
            return refine(lib, cs, outdir, filt, instr)
        iters = load_iters(outdir)
        if not iters and todo:   # choose the iteration counts once, at the latest commit, before the parallel runs
            last = todo[-1]; todo = todo[:-1]
            c, res, err = one(lib, last, filt, rp.CORES[0], instr, outdir, iters)
            save_iters(outdir, iters); print(c[:8], "ok" if res else "fail", "(iteration counts chosen here)", flush=True)
        for c, res, err in on_cores(lambda c, core: one(lib, c, filt, core, instr, outdir, iters), todo):
            save_iters(outdir, iters)
            print(c[:8], "ok" if res else "fail", flush=True)
        return
    iters = {}   # chosen at the first commit, used for the others
    for c in args[1:]:
        h = subprocess.run(["git", "-C", str(src), "rev-parse", c], capture_output=True, text=True, check=True).stdout.strip()
        _, res, err = one(lib, h, filt, rp.CORES[0], instr, iters=iters)
        print(f"== {lib} {c} ({h[:8]})")
        if res is None: print("  build failed:\n" + err[-1500:]); continue
        for name, r in res.items():
            vals = f"instr {r['instr']:.0f}" if instr else f"alloc {r['alloc']:.2f} free {r['free']:.2f} lock {r['lock']:.2f}"
            print(f"  {name[:55]:55} {vals}   {r['ns']:.1f} ns")

if __name__ == "__main__":
    main()
