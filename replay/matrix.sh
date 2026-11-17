#!/bin/bash
# The latest library commit of every project, with each compiler at -O2 and -O3, run one at a time on
# the first core of REPLAY_CORES (quill: the first two). ROUNDS passes over all builds, so that slow drift of the machine (frequency,
# other load) affects every build alike; replay/matrix_table.py takes the median over the passes.
# Output: out/<cc>-m<opt>-r<k>/<project>/.
#   replay/matrix.sh [ROUNDS]
set -u
cd "$(dirname "$0")/.."
ROUNDS=${1:-3}
for k in $(seq 1 "$ROUNDS"); do
  for p in liquibook concurrentqueue atomic_queue SPSCQueue MPMCQueue hffix spdlog fmt simdjson quill \
           readerwriterqueue unordered_dense robin-map parallel-hashmap robin-hood-hashing fast_float xxHash eigen; do
    H=$(python3 -c "import sys;sys.path.insert(0,'replay');import replay as r;print(r.commits('$p')[-1][0])")
    for cc in gcc gcc15 clang; do for o in -O2 -O3; do
      # quill's benchmark has a backend thread: it gets the first two cores of REPLAY_CORES
      REPLAY_OPT=$o REPLAY_TAG=-m$o-r$k python3 replay/replay.py $p --cc $cc --commits ${H:0:12} -j 1 | sed "s/^/round $k $p $cc $o /"
    done; done
  done
done
