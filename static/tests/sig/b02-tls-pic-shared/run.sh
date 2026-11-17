#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse lib.cpp --cflags=-fPIC -e bump -e bump_local -e outer
show bump outer inner
python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc','free','lock','syscall','tls-dynamic')}, ' trusted:', e.get('trusted'))"
$LLVM/clang -O1 -g host.c -o host -ldl -pthread
echo "--- -O0 -fPIC -shared of whole.bc, dlopen'ed by host"
$TS $CXX -O0 -g -fPIC -shared w/whole.bc -o lib0.so -w && FX_SOLIB=$PWD/lib0.so FX_ARGS=$PWD/lib0.so check ./host inner | grep -E "VIOLATION|GDB|PROGRAM|NOTRUN|RT"
echo "--- -O2 -flto -fPIC -shared of whole.bc"
$TS $CXX -O2 -g -flto -fuse-ld=lld -fPIC -shared w/whole.bc -o lib2.so -w && FX_SOLIB=$PWD/lib2.so FX_ARGS=$PWD/lib2.so check ./host inner | grep -E "VIOLATION|GDB|PROGRAM|NOTRUN|RT|NOSYM"
