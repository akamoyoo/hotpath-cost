#!/bin/bash
# hffix at its latest commit: the original header and three patched variants (patch.py), each built with
# every compiler at -O2 and -O3. Checks that every variant writes the same bytes as the original
# (same_bytes.cpp), then times the benchmark of replay/bench/hffix.cpp three times on one core.
#   cases/hffix/run.sh [CORE]          compilers from CXX_GCC, CXX_GCC15, CXX_CLANG
set -eu
cd "$(dirname "$0")/../.."
CORE=${1:-2}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
git -C src/hffix archive HEAD include | tar -x -C "$W"
for v in orig inline fast both; do
  mkdir -p "$W/$v"; cp "$W/include/hffix_fields.hpp" "$W/$v/"
  if [ $v = orig ]; then cp "$W/include/hffix.hpp" "$W/$v/"; else python3 cases/hffix/patch.py "$W/include/hffix.hpp" "$W/$v/hffix.hpp" $v; fi
done
declare -A CXXS=([gcc13]=${CXX_GCC:-g++} [gcc15]=${CXX_GCC15:-g++-15} [clang21]=${CXX_CLANG:-clang++})
echo "== same bytes (hash of 200000 messages, per compiler: all variants must agree)"
for cc in gcc13 gcc15 clang21; do for v in orig inline fast both; do
  ${CXXS[$cc]} -std=c++17 -O2 -DNDEBUG -w -I"$W/$v" cases/hffix/same_bytes.cpp -o "$W/sb"
  echo "$cc $v $("$W/sb")"
done; done
echo "== timing (ns per call, median of 3 runs on core $CORE)"
for cc in gcc13 gcc15 clang21; do for o in O2 O3; do for v in orig inline fast both; do
  ${CXXS[$cc]} -std=c++17 -$o -DNDEBUG -pthread -march=x86-64-v3 -w -I"$W/$v" -Ireplay/drivers/hffix replay/bench/hffix.cpp -o "$W/b-$cc-$o-$v" -ldl
done; done; done
for r in 1 2 3; do for b in "$W"/b-*; do
  taskset -c "$CORE" "$b" | python3 -c "import sys,json,os; [print(os.path.basename('$b'), d['op'], d['ns']) for d in map(json.loads, sys.stdin)]"
done; done | python3 cases/hffix/table.py
