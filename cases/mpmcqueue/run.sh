#!/bin/bash
# rigtorp/MPMCQueue at its latest commit, original and patched (patch.py), with every compiler, plus
# clang without its 64-bit division bypass (-Xclang -target-feature -Xclang -idivq-to-divl). Checks the
# patched queue with a 4x4-thread stress test, prints the divisions in try_push, and times
# replay/bench/mpmcqueue.cpp three times on one core.
#   cases/mpmcqueue/run.sh [CORE]          compilers from CXX_GCC, CXX_GCC15, CXX_CLANG
set -eu
cd "$(dirname "$0")/../.."
CORE=${1:-2}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
git -C src/MPMCQueue archive HEAD include | tar -x -C "$W"; mv "$W/include" "$W/orig"
mkdir -p "$W/patched/rigtorp"; python3 cases/mpmcqueue/patch.py "$W/orig/rigtorp/MPMCQueue.h" "$W/patched/rigtorp/MPMCQueue.h"
declare -A CXXS=([gcc13]=${CXX_GCC:-g++} [gcc15]=${CXX_GCC15:-g++-15} [clang21]=${CXX_CLANG:-clang++})
F="-std=c++17 -O2 -DNDEBUG -pthread -march=x86-64-v3 -w"
echo "== stress test (4 producers, 4 consumers, capacity 1000)"
for cc in gcc13 clang21; do for v in orig patched; do
  ${CXXS[$cc]} $F -I"$W/$v" cases/mpmcqueue/stress.cpp -o "$W/st"; echo "$cc $v: $("$W/st")"
done; done
for cc in gcc13 gcc15 clang21; do for v in orig patched; do
  ${CXXS[$cc]} $F -I"$W/$v" -Ireplay/drivers/mpmcqueue replay/bench/mpmcqueue.cpp -o "$W/b-$cc-$v" -ldl
done; done
${CXXS[clang21]} $F -Xclang -target-feature -Xclang -idivq-to-divl -I"$W/orig" -Ireplay/drivers/mpmcqueue replay/bench/mpmcqueue.cpp -o "$W/b-clang21-orig-nobypass" -ldl
echo "== divisions in try_push (div instructions; clang's bypass adds a 32-bit div next to each 64-bit one)"
for b in "$W"/b-*; do
  echo "$(basename "$b"): $(objdump -d --no-show-raw-insn "$b" | awk '/<_ZN8hp_entry13mpmc_try_push/{p=1} p&&/^$/{exit} p' | grep -c div)"
done
echo "== ns per call, median of 3 runs on core $CORE"
for r in 1 2 3; do for b in "$W"/b-*; do
  taskset -c "$CORE" "$b" | python3 -c "import sys,json,os; [print(os.path.basename('$b'), d['op'], d['ns']) for d in map(json.loads, sys.stdin)]"
done; done | python3 -c '
import sys, collections, statistics
t = collections.defaultdict(list)
for l in sys.stdin:
    b, op, ns = l.split(); t[(b, op)].append(float(ns))
for b in sorted({k[0] for k in t}):
    print(f"{b:26}" + "".join(f"  {op} {statistics.median(t[(b, op)]):6.1f}" for op in ["push+pop", "try_push+try_pop"]))
'
