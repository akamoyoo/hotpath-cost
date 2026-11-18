#!/usr/bin/env python3
"""Static and run-time view of one commit, side by side.

  combine.py PROJECT COMMIT [--json OUT]

Runs, on the same commit and with clang for both:
  - fn-effects (static/, the LLVM-based analyser of this repository) on the driver: for every entry
    function, the allocation and mutex sites it can reach, with the analysis' hot/cold tag;
  - the workload with call-stack recording (attribute.py): the allocations and locks that happen.

Sites are matched by the innermost frame in the library's own source (file and line). A site the
static analysis places inside a standard-library function that was not inlined has no library line;
it is matched by the library function that calls it. Each site ends up in one of three groups:
  exercised      static and run time agree; the run-time count per call is given
  not exercised  reachable according to the static analysis, never reached by the workload
  static miss    reached at run time, absent from the static result (a gap in the analysis)

Environment: FN_EFFECTS (path of the fn-effects script; default: static/fn-effects, whose plugin must be
built, see static/README.md), LLVM_BIN (added to PATH for it), CXX_CLANG, REPLAY_CORES as for replay.py. Only projects with a driver (P[...]['drv']).
"""
import collections, json, os, re, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay as rp
import attribute

CLASSES = ("alloc", "lock")
NONUSER = ("std::", "__gnu_cxx::", "__cxx", "operator new", "operator delete", "__libc", "__GI", "_IO_",
           "__cxa_", "_Unwind", "__gthread", "pthread_", "malloc", "free", "calloc", "realloc")

def is_user_file(loc):
    return bool(loc) and not loc.startswith(("/", "<", "?", "./")) and "/c++/" not in loc and not loc.startswith("replay/")

def is_user_fn(name):
    return not name.startswith(NONUSER)

def short(name):
    """A function name without its parameter list, for matching names between the two views."""
    depth = 0
    for i, ch in enumerate(name):
        if ch in "<": depth += 1
        elif ch == ">": depth -= 1
        elif ch == "(" and depth == 0 and i > 0 and not name.startswith("operator"):
            return name[:i]
    return name

def static_sites(proj, h, tree):
    cfg = rp.P[proj]
    fx = os.environ.get("FN_EFFECTS", str(rp.TOP / "static" / "fn-effects"))
    env = dict(os.environ)
    if os.environ.get("LLVM_BIN"): env["PATH"] = os.environ["LLVM_BIN"] + ":" + env["PATH"]
    with tempfile.TemporaryDirectory() as w:
        w = Path(w)
        flags = " ".join(["-O2", "-g", "-DNDEBUG", "-pthread", *[f"-I{tree / i}" for i in cfg["inc"]], *cfg.get("flags", [])])
        cmd = [sys.executable, fx, str(rp.HERE / "drivers" / cfg["drv"] / "main.cpp"), *[str(tree / s) for s in cfg["srcs"]],
               "--std=c++17", f"--cflags={flags}", "-e", "hp_entry::*", "-o", str(w / "r.json"), "--text", "/dev/null",
               "--effects", str(w / "e.json"), "--path-base", str(tree), "--pta-budget", "120"]
        p = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if not (w / "e.json").exists():
            raise RuntimeError("fn-effects failed:\n" + p.stderr[-3000:])
        funcs = json.load(open(w / "e.json"))["functions"]
    prefix = str(tree) + "/"
    rel = lambda f: f[len(prefix):] if f.startswith(prefix) else f
    out = {}
    for k, f in funcs.items():
        if not f["demangled"].startswith("hp_entry::"): continue
        # reachable functions, and for each the user-code functions through which it is reached
        seen, todo, via = set(), [(k, None)], collections.defaultdict(set)
        while todo:
            g, u = todo.pop()
            if g not in funcs: continue
            name = funcs[g]["demangled"]
            u2 = name if is_user_fn(name) else u
            if (g, u2) in seen: continue
            seen.add((g, u2)); via[g].add(u2)
            todo += [(c["callee"], u2) for c in funcs[g].get("calls", []) if c.get("callee")]
        sites = {}; by_fn = {}
        for g, users in via.items():
            gname = funcs[g]["demangled"]
            for s in funcs[g].get("own", []):
                # own and library sites; and the allocation a throw makes (__cxa_allocate_exception), which the
                # analysis records as a consequence of the throw
                if s.get("class") not in CLASSES: continue
                if s.get("account") not in ("own", "library") and not (s.get("account") == "consequence" and s.get("consequence_of") == "throw" and s.get("class") == "alloc"): continue
                loc = f"{rel(s.get('file', ''))}:{s.get('line', 0)}"
                info = dict(temp=s.get("temp", "?"), symbol=s.get("symbol", ""), evidence=s.get("evidence", ""))
                if is_user_file(rel(s.get("file", ""))) and s.get("line"):
                    key = (s["class"], "line", loc)
                    sites.setdefault(key, dict(info, function=gname))
                    # the same site by the function that contains it, for run-time stacks that lost the
                    # innermost frame (a tail call) and so name only the caller's line
                    by_fn.setdefault((s["class"], short(gname)), key)
                else:   # inside a non-inlined library-external function: key by the user caller
                    for u in users:
                        if u: sites.setdefault((s["class"], "fn", short(u)), dict(info, function=gname, loc=loc))
        out[f["demangled"]] = (sites, by_fn)
    return out

def dynamic_sites(proj, h):
    out = collections.defaultdict(dict)
    for e in attribute.trace(proj, h, "clang"):
        entry = next((fn for fn, _ in e["frames"] if fn.startswith("hp_entry::")), None)
        if entry is None: continue
        users = tuple((fn, loc.split(" ")[0]) for fn, loc in e["frames"] if is_user_file(loc.split(" ")[0]) and is_user_fn(fn))
        user = users[0] if users else None
        rec = out[entry].setdefault((e["kind"], user), dict(steady=0.0, first=0.0, stack=users, throw=False))
        rec["steady" if e["phase"] == "steady state" else "first"] += e["per_call"]
        # an allocation by __cxa_allocate_exception is a C++ exception being thrown, often caught and
        # swallowed inside the library: in ordinary use, a sign that something went wrong
        rec["throw"] |= any("__cxa_allocate_exception" in fn for fn, _ in e["frames"])
    return out

def bare_name(fn):
    """The unqualified name of a function without template arguments or parameters, as it is written at a
    call site: 'requisition_block' for 'ns::Queue<T>::requisition_block<(Mode)0>(...)'."""
    out, depth = [], 0
    for ch in short(fn):
        if ch == "<": depth += 1
        elif ch == ">": depth -= 1
        elif depth == 0: out.append(ch)
    return "".join(out).split("::")[-1].strip()

def match(kind, user, stack, s, by_fn, source):
    """The static site a run-time event corresponds to, and by which rule:
      line     the innermost library frame is the static site's line
      tail     that frame's source line names the function that holds the static site (the callee's own
               frame is missing from the stack, as after a tail call)
      caller   an outer library frame is the static site's line (the static analysis records an
               inlined callee's site at its caller's line)
      library  the site is inside a standard-library function called from the innermost library function
      function the innermost library frame is in the function that holds the static site, at another line
               (the frame of an inlined callee can come back without its name); the loosest rule, last"""
    if not user: return None, None
    if (kind, "line", user[1]) in s: return (kind, "line", user[1]), "line"
    text = source(user[1])
    for (k, fn), key in by_fn.items():
        name = bare_name(fn)
        if k == kind and name and re.search(r"\b" + re.escape(name) + r"\s*(<[^;(){}]*>)?\s*\(", text or ""):
            return key, "tail"
    for fn, loc in stack[1:]:
        if (kind, "line", loc) in s: return (kind, "line", loc), "caller"
    if (kind, "fn", short(user[0])) in s: return (kind, "fn", short(user[0])), "library"
    if (kind, short(user[0])) in by_fn: return by_fn[(kind, short(user[0]))], "function"
    return None, None

def main():
    proj, h = sys.argv[1], sys.argv[2]
    full = subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "rev-parse", h], capture_output=True, text=True, check=True).stdout.strip()
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        subprocess.run(f"git -C {rp.TOP / 'src' / proj} archive {full} | tar -x -C {t}", shell=True, check=True)
        st = static_sites(proj, full, t)
    dy = dynamic_sites(proj, full)
    cache = {}
    def source(loc):
        path, _, line = loc.rpartition(":")
        if path not in cache:
            cache[path] = subprocess.run(["git", "-C", str(rp.TOP / "src" / proj), "show", f"{full}:{path}"],
                                         capture_output=True, text=True).stdout.splitlines()
        try: return cache[path][int(line) - 1]
        except (ValueError, IndexError): return ""
    report = {}; totals = [0, 0, 0, 0]
    for entry in sorted(set(st) | set(dy)):
        s, by_fn = st.get(entry, ({}, {})); d = dy.get(entry, {})
        used = set(); exercised = []; miss = []
        for (kind, user), cnt in sorted(d.items(), key=lambda kv: str(kv[0])):
            hit, how = match(kind, user, cnt.get("stack", ()), s, by_fn, source)
            row = dict(kind=kind, function=user[0] if user else "?", loc=user[1] if user else "?", how=how,
                       steady=cnt["steady"], first=cnt["first"], throw=cnt.get("throw", False))
            if hit: used.add(hit); exercised.append(row)
            else: miss.append(row)
        notex = [dict(kind=k[0], key=k[2], **v) for k, v in sorted(s.items()) if k not in used]
        n_static = sum(1 for v in s.values() if v.get("temp") != "c")
        n_reached = sum(1 for k in used if s.get(k, {}).get("temp") != "c")
        report[entry] = dict(exercised=exercised, not_exercised=notex, static_miss=miss,
                             hot_sites=n_static, hot_sites_reached=n_reached)
        totals[0] += n_static; totals[1] += n_reached; totals[2] += len(miss)
        totals[3] += sum(1 for r in exercised + miss if r.get("throw"))
        print(f"== {entry}")
        if n_static: print(f"   reached {n_reached} of the {n_static} static sites not marked cold")
        if entry not in dy:
            n_hot = sum(1 for r in notex if r["temp"] != "c")
            print(f"   no allocation or lock at run time (or not called); static: {n_hot} sites not marked cold, {len(notex) - n_hot} cold")
            for r in [r for r in notex if r["temp"] != "c"]:
                where = r['key'] if ":" in r['key'] else "in " + short(r['function'])[:40]
                print(f"      {r['kind']:5} {where[:48]:48} {('via ' + short(r['key'])) if ':' not in r['key'] else short(r['function'])[:60]:60} [{r['temp']}]")
            continue
        print(f"   exercised ({len(exercised)}):")
        for r in exercised:
            print(f"      {r['kind']:5} {r['loc']:32} {short(r['function'])[:52]:52} per call {r['steady']:.2f}, first call {r['first']:.0f}  ({r['how']})"
                  + ("   <- AN EXCEPTION thrown inside the library" if r.get("throw") else ""))
        hot = [r for r in notex if r["temp"] != "c"]; cold = [r for r in notex if r["temp"] == "c"]
        print(f"   not exercised: {len(hot)} not marked cold, {len(cold)} cold (error or unlikely paths)")
        for r in hot:
            where = r['key'] if ":" in r['key'] else "in " + short(r['function'])[:40]
            print(f"      {r['kind']:5} {where[:48]:48} {('via ' + short(r['key'])) if ':' not in r['key'] else short(r['function'])[:60]:60} [{r['temp']}]")
        print(f"   static miss ({len(miss)}):")
        for r in miss:
            print(f"      {r['kind']:5} {r['loc']:32} {short(r['function'])[:60]:60} per call {r['steady']:.2f}, first call {r['first']:.0f}")
    print(f"all entries: reached {totals[1]} of {totals[0]} static sites not marked cold; {totals[2]} static misses")
    if totals[3]:
        print(f"{totals[3]} run-time site(s) are exceptions thrown inside the library (__cxa_allocate_exception). In ordinary\n"
              "use that is a sign of a fault; look at the stack: attribute.py LIB COMMIT --cc clang --json")
    if "--json" in sys.argv:
        json.dump(report, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)

if __name__ == "__main__":
    main()
