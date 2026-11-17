#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp --cflags=-femulated-tls -e outer
show _Z4bumpl _Z5touchll _Z5outerl
python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc','free','lock','syscall','tls-dynamic')}, ' trusted:', e.get('trusted'))"
llvm-dis w/whole.bc -o - | grep -n "emutls\|thread_local" | head -5
echo "--- -O0 of whole.bc, code generation with the same flag (-femulated-tls)"
build0 -femulated-tls && check ./t0 bump touch outer | grep -E "VIOLATION|GDB|PROGRAM|NOTRUN"
echo "--- -O2 -flto link of whole.bc, -femulated-tls"
build2 -femulated-tls && check ./t2 bump touch outer | grep -E "VIOLATION|GDB|PROGRAM|NOTRUN|NOSYM"
llvm-objdump -d --no-show-raw-insn t0 --disassemble-symbols=_Z4bumpl | grep -E "call|emutls"
