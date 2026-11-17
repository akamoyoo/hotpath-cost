#!/bin/bash
# asm.sh COMMIT gcc|clang ENTRY : machine code of one entry of replay/drivers/atomic_queue/main.cpp at COMMIT
# (compilers from CXX_GCC, CXX_CLANG; -O2 -march=x86-64-v3)
set -eu
cd "$(dirname "$0")/../.."
C=$1; CC=$2; F=$3
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
git -C src/atomic_queue archive "$C" include | tar -x -C "$W"
case $CC in gcc) X=${CXX_GCC:-g++};; clang) X=${CXX_CLANG:-clang++};; esac
$X -std=c++17 -O2 -DNDEBUG -pthread -march=x86-64-v3 -w -I"$W/include" -Ireplay/drivers/atomic_queue \
   -x c++ - -o "$W/d" <<<'#include "main.cpp"'
objdump -d -C --no-show-raw-insn "$W/d" | awk -v f="<hp_entry::$F(" 'index($0,f)>0 && /:$/{p=1;next} p&&/^$/{exit} p'
