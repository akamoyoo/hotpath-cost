#!/usr/bin/env python3
"""Where does a compiler gap come from? Machine-code facts of one entry function across builds.

  gap.py PROJECT ENTRY [--commit H] [--builds gcc:-O2,gcc:-O3,gcc15:-O2,gcc15:-O3,clang:-O2,clang:-O3]
  gap.py PROJECT ENTRY --commits A,B [--builds gcc:-O2]      the same facts for two commits, one build

ENTRY is a substring of the entry function's name (for example `fix_write_new_order`). For each build
the workload is compiled at the commit (default: the latest library commit) and disassembled. For the
entry and every function of the program it reaches by direct calls, the tool counts instructions,
call sites, divisions, 256-bit vector instructions and loops (backward jumps), and lists the functions
that stay out of line. The median ns of the compiler matrix is shown when replay/matrix.sh has run.

The point is triage, not proof. When one build is much faster than another, the difference is usually
visible as one of these: a helper that is out of line in the slow build (hffix: details::itoa), an
extra division (MPMCQueue), or vector code in one build only. Confirm by reading the code, and price
the cause by timing a variant.
"""
import collections, json, os, re, statistics, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay as rp

LIBRARY = ("std::", "__gnu_cxx::", "operator new", "operator delete", "__cxa", "_Unwind", "__libc", "__stack_chk")

def disasm(exe):
    out = subprocess.run(["objdump", "-d", "-C", "--no-show-raw-insn", str(exe)], capture_output=True, text=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <(.*)>:$", line)
        if m:
            cur = m.group(2); funcs[cur] = dict(addr=int(m.group(1), 16), ins=[]); continue
        if cur and "\t" in line:
            a = line.split(":", 1)[0].strip()
            try: funcs[cur]["ins"].append((int(a, 16), line.split("\t", 1)[1].strip()))
            except ValueError: pass
    return funcs

def tree_facts(funcs, entry):
    seen, todo = [], [entry]; sites = collections.Counter()
    while todo:
        g = todo.pop()
        if g in seen or g not in funcs: continue
        seen.append(g)
        for _, ins in funcs[g]["ins"]:
            # calls, and jumps into split-off parts of a function ([clone .cold], tail calls)
            m = re.match(r"(call|j)\w*\s+[0-9a-f]+ <(.+?)(\+0x[0-9a-f]+)?>$", ins)
            if m and not m.group(2).endswith("@plt") and m.group(2) != g and (not m.group(3) or "[clone" in m.group(2)):
                todo.append(m.group(2))
                if m.group(1) == "call": sites[m.group(2)] += 1
    f = collections.Counter(); outline = []
    for g in seen:
        ins = funcs[g]["ins"]; start = funcs[g]["addr"]
        f["instructions"] += len(ins)
        for a, i in ins:
            op = i.split()[0] if i else ""
            if op.startswith("call"): f["call sites"] += 1
            if re.match(r"(i?div)", op): f["divisions"] += 1
            if "%ymm" in i: f["256-bit vector"] += 1
            m = re.match(r"j\w*\s+([0-9a-f]+) <", i)
            if m and start <= int(m.group(1), 16) < a: f["backward jumps"] += 1
        if g != entry and not g.startswith(LIBRARY) and "@plt" not in g:
            if "[clone .cold]" not in g: outline.append((re.sub(r"\(.*", "", g), sites[g]))
    return f, outline

def matrix_ns(proj, cc, opt):
    vals = collections.defaultdict(list)
    for d in sorted((rp.TOP / "out").glob(f"{cc}-m{opt}-r*")):
        for fjs in (d / proj).glob("*.jsonl") if (d / proj).exists() else []:
            for r in map(json.loads, fjs.read_text().splitlines()):
                if "ns" in r: vals[r["op"]].append(r["ns"])
    return {op: statistics.median(v) for op, v in vals.items()}

def main():
    proj, want = sys.argv[1], sys.argv[2]
    commit = sys.argv[sys.argv.index("--commit") + 1] if "--commit" in sys.argv else rp.commits(proj)[-1][0]
    commit = subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", commit], capture_output=True, text=True, check=True).stdout.strip()
    spec = sys.argv[sys.argv.index("--builds") + 1] if "--builds" in sys.argv else "gcc:-O2,gcc:-O3,gcc15:-O2,gcc15:-O3,clang:-O2,clang:-O3"
    builds = [tuple(b.split(":")) for b in spec.split(",")]
    full = lambda c: subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", c], capture_output=True, text=True, check=True).stdout.strip()
    if "--commits" in sys.argv:   # one build, two commits
        cs = sys.argv[sys.argv.index("--commits") + 1].split(",")
        cc, opt = builds[0] if "--builds" in sys.argv else ("gcc", "-O2")
        jobs = [(f"{c[:8]} {cc} {opt}", cc, opt, full(c)) for c in cs]
    else:
        jobs = [(f"{cc} {opt}", cc, opt, commit) for cc, opt in builds]
    rows = []
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        for k, (name, cc, opt, c) in enumerate(jobs):
            d = t / f"b{k}"; d.mkdir(); exe = d / "x"
            rp.OPT = opt
            b = rp.build(proj, cc, c, d, exe)
            if b.returncode: print(f"{name}: build failed"); continue
            funcs = disasm(exe)
            cands = [g for g in funcs if re.match(r"(\S+\s+)*hp_entry::", g) and want in g]   # templates carry a return type
            entry = next((g for g in cands if "[clone" not in g), cands[0] if cands else None)
            if entry is None: sys.exit(f"no entry function containing {want!r}")
            facts, outline = tree_facts(funcs, entry)
            rows.append((name, facts, outline, matrix_ns(proj, cc, opt) if "--commits" not in sys.argv else {}))
    print(f"{proj} {commit[:8] if '--commits' not in sys.argv else 'two commits'}, entry containing {want!r}\n")
    keys = ["instructions", "call sites", "divisions", "256-bit vector", "backward jumps"]
    print(f"{'build':26}" + "".join(f"{k:>16}" for k in keys) + "   out-of-line library functions")
    for name, f, outline, ns in rows:
        print(f"{name:26}" + "".join(f"{f[k]:>16}" for k in keys) + f"   {len(outline)}")
    if rows and rows[0][3]:
        print("\nmedian ns per call in the compiler matrix:")
        ops = list(rows[0][3])
        for op in ops: print(f"  {op[:40]:40}" + "".join(f"{r[3].get(op, float('nan')):>10.1f}" for r in rows))
    calls = [dict(o) for _, _, o, _ in rows]
    every = collections.Counter(g for c in calls for g in c)
    print("\nlibrary functions out of line in some builds only (call sites in the tree per build):")
    for g, n in sorted(every.items(), key=lambda kv: (-sum(c.get(kv[0], 0) for c in calls), kv[0])):
        if n < len(rows):
            print(f"  {g[:90]}\n      " + ", ".join(f"{r[0]}: {c[g]}" for r, c in zip(rows, calls) if g in c))

if __name__ == "__main__":
    main()
