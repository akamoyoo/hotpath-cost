#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
analyse a.cpp
show _Z7publishi _Z2mkl _Z2rmP _Z7mk_slabl _Z4mk_qv
build0 && build2
echo "--- -O2 -flto"; check2 ./t2 publish _Z2mkl _Z2rmP mk_slab mk_q
echo "--- -O0, only primitives that live in a library count"; check ./t0 publish _Z2mkl _Z2rmP mk_slab mk_q
echo "--- strace of the whole -O0 run: sendto/brk/mmap after startup"; strace -f -e trace=sendto,sendmsg,write ./t0 2>&1 | tail -3
echo "--- ltrace-free count: malloc/free calls in the whole run (gdb)"; gdb -q -batch -ex 'break main' -ex run -ex 'break malloc' -ex 'break free' -ex 'break _Znwm' -ex continue ./t0 2>&1 | grep -E "Breakpoint [2-9]|exited" | head
