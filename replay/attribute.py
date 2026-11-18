#!/usr/bin/env python3
"""Where each allocation and mutex lock of one call comes from, at one commit.

  attribute.py PROJECT COMMIT [--cc gcc|gcc15|clang] [--all-frames] [--json]

Builds the workload at COMMIT with -g, runs every operation with call-stack recording on
(COUNTS_TRACE=1): its first call, then 1000 more calls after the timed loop. Prints each distinct stack
with its count per call, symbolized with addr2line (inlined frames included). Standard-library frames
are left out unless --all-frames is given. --json prints the events with every frame instead.
"""
import json, os, re, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay as rp

SKIP = ("/c++/", "bits/", "counts.h", "workload.h", "libc-start", "csu/", "??:")

def trace(proj, h, cc="gcc"):
    """Events of one traced run: dicts with op, phase ('first call' or 'steady state'), kind ('alloc' or
    'lock'), per_call, and frames [(function, file:line)], innermost first, paths relative to the
    commit's tree or to this repository."""
    full = subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", h], capture_output=True, text=True, check=True).stdout.strip()
    events = []
    with tempfile.TemporaryDirectory() as t:
        t = Path(t); exe = t / "_workload_exe"
        b = rp.build(proj, cc, full, t, exe, extra=("-g", "-DHP_ENTRY_KEEP_FRAME"))
        if b.returncode: raise RuntimeError(b.stderr[:3000])
        cmd = ["taskset", "-c", ",".join(map(str, rp.CORES[:rp.P[proj].get("cores", 1)])), str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, COUNTS_TRACE="1"), timeout=900)
        op = phase = None; cur = None
        def symbolize(obj, off):
            a = subprocess.run(["addr2line", "-f", "-C", "-i", "-e", obj, off], capture_output=True, text=True).stdout.splitlines()
            return [(fn, loc.replace(str(t) + "/", "").replace(str(rp.TOP) + "/", "").split(" (")[0]) for fn, loc in zip(a[0::2], a[1::2])]
        for line in r.stderr.splitlines():
            m = re.match(r"\s+(\S+) (0x[0-9a-f]+)$", line)
            if m and cur is not None:
                cur["frames"] += symbolize(*m.groups()); continue
            m = re.match(r"== (.*), (first call|steady state)$", line)
            if m: op, phase = m.groups(); cur = None; continue
            m = re.match(r"([al]) ([0-9.]+) per call$", line)
            if m:
                cur = dict(op=op, phase=phase, kind="alloc" if m.group(1) == "a" else "lock", per_call=float(m.group(2)), frames=[])
                events.append(cur)
    return events

def main():
    proj, h = sys.argv[1], sys.argv[2]
    cc = sys.argv[sys.argv.index("--cc") + 1] if "--cc" in sys.argv else "gcc"
    every = "--all-frames" in sys.argv
    try:
        events = trace(proj, h, cc)
    except RuntimeError as e:
        sys.exit(str(e))
    if "--json" in sys.argv:
        print(json.dumps(events, indent=1)); return
    last = None
    for e in events:
        if (e["op"], e["phase"]) != last:
            print(f"== {e['op']}, {e['phase']}"); last = (e["op"], e["phase"])
        throw = any("__cxa_allocate_exception" in fn for fn, _ in e["frames"])
        print(("  allocation " if e["kind"] == "alloc" else "  mutex lock ") + f"{e['per_call']:.3f} per call"
              + ("   <- an exception thrown here (__cxa_allocate_exception)" if throw else ""))
        for fn, loc in e["frames"]:
            if not every and (any(s in loc for s in SKIP) or fn.startswith(("operator new", "std::", "__gnu_cxx"))): continue
            print(f"      {fn[:100]}  {loc}")

if __name__ == "__main__":
    main()
