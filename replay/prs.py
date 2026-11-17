#!/usr/bin/env python3
"""Check a library's open pull requests for hot-path costs.

  prs.py PROJECT [--limit N] [--all] [--no-instr]

Lists the open pull requests of the GitHub repository the clone in src/PROJECT was cloned from (with
the gh command), keeps those that touch the project's paths in P, fetches each head as
refs/hp/pr<N>, and runs check.py from its merge base with the target branch. Prints one line per pull
request and the full report of those where a count or instructions changed. --all prints every report.
Nothing is posted anywhere.
"""
import json, re, subprocess, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import replay as rp

def main():
    proj = sys.argv[1]
    limit = sys.argv[sys.argv.index("--limit") + 1] if "--limit" in sys.argv else "30"
    src = rp.TOP / "src" / proj
    url = subprocess.run(["git", "-C", str(src), "remote", "get-url", "origin"], capture_output=True, text=True).stdout.strip()
    m = re.search(r"github\.com[:/]([^/]+/[^/.]+)", url)
    if not m: sys.exit(f"{proj}: origin is not a GitHub repository ({url})")
    repo = m.group(1)
    prs = json.loads(subprocess.run(["gh", "pr", "list", "--repo", repo, "--state", "open", "--limit", limit,
                                     "--json", "number,title,baseRefName,files,updatedAt"],
                                    capture_output=True, text=True, check=True).stdout)
    paths = rp.P[proj]["paths"]
    touches = lambda f: any(f == p or f.startswith(p.rstrip("/") + "/") for p in paths)
    extra = ["--no-instr"] if "--no-instr" in sys.argv else []
    print(f"{proj} ({repo}): {len(prs)} open pull requests, checking those that touch {', '.join(paths)}")
    for pr in prs:
        if not any(touches(f["path"]) for f in pr.get("files", [])): continue
        n, target = pr["number"], pr["baseRefName"]
        r = subprocess.run(["git", "-C", str(src), "fetch", "-q", "-f", "origin", f"pull/{n}/head:refs/hp/pr{n}",
                            f"{target}:refs/hp/target-{target}"], capture_output=True, text=True)
        if r.returncode:
            print(f"#{n}: fetch failed: {r.stderr.strip()[:200]}"); continue
        base = subprocess.run(["git", "-C", str(src), "merge-base", f"refs/hp/pr{n}", f"refs/hp/target-{target}"],
                              capture_output=True, text=True).stdout.strip()
        if not base:
            print(f"#{n}: no merge base with {target}"); continue
        r = subprocess.run([sys.executable, str(HERE / "check.py"), proj, base, f"refs/hp/pr{n}", *extra],
                           capture_output=True, text=True)
        what = {0: "no change", 1: "COUNT CHANGED", 2: "instructions changed", 3: "build or run failed"}.get(r.returncode, f"exit {r.returncode}")
        print(f"#{n} [{pr['updatedAt'][:10]}] {pr['title'][:70]}: {what}", flush=True)
        if r.returncode in (1, 2) or "--all" in sys.argv:
            print("    " + r.stdout.strip().replace("\n", "\n    "), flush=True)
        elif r.returncode == 3:
            print("    " + (r.stderr.strip().splitlines() or ["?"])[0][:200])

if __name__ == "__main__":
    main()
