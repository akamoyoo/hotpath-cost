#!/usr/bin/env python3
"""Check the tool against results it is known to give. Run it after changing the tool.

  selftest.py [--quick] [--no-static]

Builds and measures a few commits of libraries in src/ in a temporary output directory (out/ is not
touched) and compares exact counts with the values the documents report. --quick skips the slower
checks (history replay, exploration); --no-static skips the ones that need static/ built and clang.
Exits non-zero if any check fails.
"""
import json, os, subprocess, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve().parent

CHECKS = [
    # (what, command, expected substrings in the output)
    ("Liquibook 2.0.0 allocates 4 times per add+cancel",
     ["replay.py", "liquibook", "--commits", "25b0c5ec,dd5ada83"], None),
    ("msgpack-c: 2 allocations per unpack into a reused zone",
     ["replay.py", "msgpack-c", "--commits", "bcba2717"], None),
    ("explain.py names callback_now as the new allocation",
     ["explain.py", "liquibook", "25b0c5ec", "492c842c"], ["alloc   1.00 ->   4.00 per call in total", "callback_now"]),
    ("Boost.Unordered before unordered_flat_map existed does not build (installed headers hidden)",
     ["replay.py", "boost-unordered", "--commits", "29660f9c"], ["build-fail"]),
    ("abseil builds with trapping stubs for its sampler and measures 0 allocations",
     ["replay.py", "abseil", "--commits", "c16d5557"], None),
]
COUNTS = {   # (project, commit prefix, operation): (alloc, lock)
    ("liquibook", "25b0c5ec", "add+cancel"): (4.0, 0.0),
    ("liquibook", "dd5ada83", "add+cancel"): (2.0, 0.0),
    ("msgpack-c", "bcba2717", "unpack, reused zone"): (2.0, 0.0),
    ("msgpack-c", "bcba2717", "pack, reused buffer"): (0.0, 0.0),
    ("abseil", "c16d5557", "insert+erase"): (0.0, 0.0),
}
SLOW = [
    ("diff.py pins concurrentqueue's block frees to a15a7236",
     [["replay.py", "concurrentqueue-explore", "--commits", "9c44f36d,a15a7236"], ["diff.py", "concurrentqueue-explore", "--tag", "{tag}"]],
     ["a15a7236", "alloc 0.13 -> 1.05"]),
]
SLOW += [
    ("check.py: abseil f2f4fa2c changes instructions only, +50 % per operator[]",
     ["check.py", "abseil", "d5ccaa21", "f2f4fa2c"], ["+49.9 %", "no count changed; instructions changed"]),
]
SLOW += [
    ("bench.py: spdlog's own benchmarks, a null sink locks nothing and a multi-threaded sink once",
     ["bench.py", "spdlog", "6d9805df", "--filter", "^(null_sink_st|/dev/null_mt/real_time)$"],
     ["null_sink_st                                            alloc 0.00 free 0.00 lock 0.00",
      "/dev/null_mt/real_time                                  alloc 0.00 free 0.00 lock 1.00"]),
]
STATIC = [
    ("combine.py: Liquibook's fixed workload reaches 5 of 85 hot sites, no static miss",
     ["combine.py", "liquibook", "25b0c5ec"], ["reached 5 of 85 static sites not marked cold; 0 static misses"]),
    ("combine.py: the random order flow reaches 14 of 85 and flags the exception behind Liquibook #44",
     ["combine.py", "liquibook-explore", "2427613b32"], ["reached 14 of 85", "AN EXCEPTION thrown inside the library"]),
]

def run(cmd, env):
    r = subprocess.run([sys.executable, str(HERE / cmd[0]), *cmd[1:]], capture_output=True, text=True, env=env, timeout=3600)
    return r.returncode, r.stdout + r.stderr

def main():
    quick, static = "--quick" in sys.argv, "--no-static" not in sys.argv
    fails = 0
    if True:
        # results go to out/<cc>-selftest-<pid>/, removed at the end
        tag = f"-selftest-{os.getpid()}"
        env = dict(os.environ, REPLAY_TAG=tag)
        def check(what, cmds, want):
            nonlocal fails
            out = ""
            for c in (cmds if isinstance(cmds[0], list) else [cmds]):
                rc, o = run([a.replace("{tag}", tag) for a in c], env); out += o
                if rc: break
            ok = (rc == 0 or (c[0] == "check.py" and rc in (1, 2))) and all(w in out for w in (want or []))
            print(f"{'ok  ' if ok else 'FAIL'} {what}")
            if not ok: fails += 1; print("     " + out[-1500:].replace("\n", "\n     "))
        for what, cmd, want in CHECKS: check(what, cmd, want)
        top = HERE.parent
        for (proj, c, op), (a, l) in COUNTS.items():
            files = list((top / "out" / f"gcc{tag}" / proj).glob(f"{c}*.jsonl"))
            got = next((d for f in files for d in map(json.loads, f.read_text().splitlines()) if d["op"] == op), None)
            ok = got is not None and abs(got["alloc"] - a) < 0.01 and abs(got["lock"] - l) < 0.01
            print(f"{'ok  ' if ok else 'FAIL'} {proj} {c} {op}: alloc {a}, lock {l}" + ("" if ok else f" (got {got})"))
            fails += not ok
        if not quick:
            for what, cmds, want in SLOW:
                if "check.py" in str(cmds) and not os.environ.get("VALGRIND"): print(f"skip {what} (VALGRIND not set)"); continue
                if "bench.py" in str(cmds) and not os.environ.get("GBENCH_DIR"): print(f"skip {what} (GBENCH_DIR not set)"); continue
                check(what, cmds, want)
        if static:
            for what, cmd, want in STATIC: check(what, cmd, want)
        subprocess.run(["rm", "-rf", *[str(p) for p in (top / "out").glob(f"*{tag}*")]])
    print("all checks passed" if not fails else f"{fails} checks failed")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
