#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp -e 'sys_*' -e cond_jump_call
show sys_ cond_jump
python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], 'syscall =', e['verdict'].get('syscall'), ' trusted:', e.get('trusted'), ' assumptions:', str(e.get('assumptions'))[:200])"
build0 || exit
echo "--- run-time: strace -f, getpid system calls made by ./t0 (expect 5: one per function), exit status"
strace -f -e trace=getpid ./t0 2>&1 | grep -c "getpid()"; ./t0; echo "exit=$?"
echo "--- per function: gdb, catch syscall getpid, frame at each entry into the kernel"
timeout 60 gdb -q -batch -x catch.py ./t0 2>&1 | grep "^SYSCALL"
