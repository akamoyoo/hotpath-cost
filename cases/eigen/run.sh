#!/bin/bash
# Eigen's 4x4 Cholesky solve (replay/bench/eigen.cpp, eg_solve4): instructions per call around the
# commit that introduced the masked load/store framework, the time of the two commits and of the two
# releases under each compiler, and the machine-code facts of the two commits.
#   cases/eigen/run.sh [CORE]    (VALGRIND, VALGRIND_INC, CXX_GCC15, CXX_CLANG as for replay/)
set -eu
cd "$(dirname "$0")/../.."
CORE=${1:-2}
python3 replay/replay.py eigen --metric instr --commits cebe0911,28c3b26d > /dev/null
python3 replay/diff.py eigen --instr 5 | grep -A4 "^28c3b26d" || true
for cc in gcc gcc15 clang; do
  python3 replay/price.py eigen cebe0911 28c3b26d --cc $cc --core "$CORE" --rounds 9 | grep -E -- "->|Cholesky"
done
for cc in gcc clang; do
  python3 replay/price.py eigen 3.4.1 5.0.1 --cc $cc --core "$CORE" --rounds 9
done
python3 replay/gap.py eigen eg_solve4 --commits cebe0911,28c3b26d
