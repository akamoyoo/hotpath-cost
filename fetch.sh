#!/bin/bash
# Clones the libraries into src/ with full history (about 3 GB). Nothing else is downloaded.
set -eu
cd "$(dirname "$0")"; mkdir -p src
while read name url; do
  [ -d "src/$name" ] || git clone -q "$url" "src/$name"
  echo "$name $(git -C src/$name rev-parse --short HEAD) $(git -C src/$name log -1 --format=%cd --date=short)"
done <<'LIST'
liquibook https://github.com/enewhuis/liquibook
quill https://github.com/odygrd/quill
concurrentqueue https://github.com/cameron314/concurrentqueue
atomic_queue https://github.com/max0x7ba/atomic_queue
SPSCQueue https://github.com/rigtorp/SPSCQueue
MPMCQueue https://github.com/rigtorp/MPMCQueue
hffix https://github.com/jamesdbrock/hffix
spdlog https://github.com/gabime/spdlog
fmt https://github.com/fmtlib/fmt
simdjson https://github.com/simdjson/simdjson
readerwriterqueue https://github.com/cameron314/readerwriterqueue
unordered_dense https://github.com/martinus/unordered_dense
robin-map https://github.com/Tessil/robin-map
parallel-hashmap https://github.com/greg7mdp/parallel-hashmap
robin-hood-hashing https://github.com/martinus/robin-hood-hashing
fast_float https://github.com/fastfloat/fast_float
xxHash https://github.com/Cyan4973/xxHash
eigen https://gitlab.com/libeigen/eigen.git
json https://github.com/nlohmann/json
taskflow https://github.com/taskflow/taskflow
pdqsort https://github.com/orlp/pdqsort
lz4 https://github.com/lz4/lz4
dragonbox https://github.com/jk-jeon/dragonbox
libdivide https://github.com/ridiculousfish/libdivide
date https://github.com/HowardHinnant/date
LIST
ln -sfn liquibook src/liquibook-explore             # exploration workloads run on the same clones
ln -sfn concurrentqueue src/concurrentqueue-explore
