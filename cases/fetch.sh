#!/bin/bash
# Fetch the three open-source projects of cases/README.md at the exact commits the cases were written for.
# The sources are only compiled to LLVM bitcode and analysed; nothing of them is executed.
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p src
get() {  # name url commit
  [ -d "src/$1/.git" ] && { echo "$1: already there"; return; }
  mkdir -p "src/$1"
  git -C "src/$1" init -q
  git -C "src/$1" remote add origin "$2"
  git -C "src/$1" fetch -q --depth 1 origin "$3"
  git -C "src/$1" checkout -q FETCH_HEAD
  echo "$1: $(git -C "src/$1" rev-parse HEAD)"
}
get concurrentqueue https://github.com/cameron314/concurrentqueue 6dd38b8a1dbaa7863aa907045f32308a56a6ff5d   # tag v1.0.4
get liquibook       https://github.com/enewhuis/liquibook         e1970483960fc3ea96e220c490a1d4c2b81e165f   # tag 2.0.0
get tcpshm          https://github.com/MengRao/tcpshm             f8258162beb75e00e4278110db4dbcb2225a1c15   # master, no tags
