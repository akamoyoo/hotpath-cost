#!/bin/bash
# hffix encoder with GCC at -O2, -O2 plus each of the inlining parameters that -O3 changes, all five
# together, and -O3: instructions of the encoder (its split-off parts included), out-of-line calls to
# itoa/utoa, and ns per message (median of three interleaved runs on CORE).
#   cases/hffix/inline_params.sh [CORE]
set -eu
cd "$(dirname "$0")/../.."
CORE=${1:-2}; CXX=${CXX_GCC:-g++}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
git -C src/hffix archive HEAD include | tar -x -C "$W"
ALL="--param=max-inline-insns-auto=30 --param=early-inlining-insns=14 --param=inline-min-speedup=15 --param=max-inline-insns-single=200 --param=inline-heuristics-hint-percent=600"
V=("-O2" "-O2 --param=max-inline-insns-auto=30" "-O2 --param=early-inlining-insns=14" "-O2 --param=inline-min-speedup=15"
   "-O2 --param=max-inline-insns-single=200" "-O2 --param=inline-heuristics-hint-percent=600" "-O2 $ALL" "-O3")
for i in "${!V[@]}"; do
  $CXX -std=c++17 ${V[$i]} -DNDEBUG -pthread -march=x86-64-v3 -w -I"$W/include" -Ireplay/drivers/hffix replay/workloads/hffix.cpp -o "$W/b$i" -ldl
done
for r in 1 2 3; do for i in "${!V[@]}"; do
  echo "$i $(taskset -c "$CORE" "$W/b$i" | head -1 | python3 -c 'import sys,json;print(json.loads(sys.stdin.read())["ns"])')"
done; done > "$W/t"
for i in "${!V[@]}"; do
  objdump -d --no-show-raw-insn "$W/b$i" > "$W/s"
  python3 - "$W/s" "$(awk -v j=$i '$1==j{print $2}' "$W/t" | sort -n | sed -n 2p)" "${V[$i]}" <<'PY'
import re, sys
insns = 0; calls = 0
for blk in open(sys.argv[1]).read().split("\n\n"):
    if not re.search(r"<_ZN8hp_entry19fix_write_new_order[^>]*>:", blk): continue
    for l in blk.splitlines()[1:]:
        if "\t" not in l: continue
        insns += 1
        if re.search(r"call\w*\s+\S+ <[^>]*(itoa|utoa)", l): calls += 1
print(f"{float(sys.argv[2]):7.0f} ns  {insns:5} insns  {calls:2} itoa/utoa calls  {sys.argv[3]}")
PY
done
