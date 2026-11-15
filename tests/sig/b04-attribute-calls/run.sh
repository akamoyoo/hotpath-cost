#!/bin/bash
cd "$(dirname "$0")"; . ../common.sh
verdicts() {
  show _Z4leafl _Z3hotl
  python3 -c "
import json
r = json.load(open('report.json'))
for e in r['entries']: print('ENTRY', e['demangled'], {c: e['verdict'].get(c) for c in ('alloc','lock','syscall','external-unknown')}, ' trusted:', e.get('trusted'))"
}
for fl in "-pg" "-finstrument-functions" "-fsplit-stack" "-fsanitize=safe-stack"; do
  echo "=== --cflags='$fl'   (static only: how the attribute is treated)"; analyse a.cpp "--cflags=$fl" -e hot; verdicts
done
echo "=== --cflags='-pg -mfentry'   (attribute \"fentry-call\"=\"true\": code generation puts 'call __fentry__' at every function entry)"
analyse a.cpp "--cflags=-pg -mfentry" -e hot; verdicts
$LLVM/clang -O1 -c hook.c -o hook.o
build0 hook.o && llvm-objdump -d --no-show-raw-insn t0 --disassemble-symbols=_Z4leafl | grep -m2 -E "call|leaf>:"
echo "--- -O0 of whole.bc + hook.o (a tracer's __fentry__ that logs with write)"; check ./t0 leaf hot 2>/dev/null | grep -E "VIOLATION|GDB"
build2 hook.o && echo "--- -O2 -flto of whole.bc + hook.o" && check ./t2 leaf hot 2>/dev/null | grep -E "VIOLATION|GDB|NOSYM"
echo "=== --cflags='-fxray-instrument -fxray-instruction-threshold=1'   (sleds patched by the XRay runtime the same flag links in)"
analyse a.cpp "--cflags=-fxray-instrument -fxray-instruction-threshold=1" -e hot; verdicts
build0 -fxray-instrument
cat > xray_gdb.py <<'PY'
import gdb
gdb.execute("set pagination off"); gdb.execute("set environment XRAY_OPTIONS=patch_premain=true xray_mode=xray-basic xray_logfile_base=xr. verbosity=0")
gdb.execute("break main"); gdb.execute("run many"); gdb.execute("catch syscall mmap write")
seen = set()
for _ in range(60):
    try: gdb.execute("continue", to_string=True)
    except gdb.error: break
    try: f = gdb.selected_frame()
    except gdb.error: break
    names = []
    while f is not None and len(names) < 12: names.append(f.name() or "?"); f = f.older()
    mod = [n for n in names if n in ("leaf", "hot")]
    if mod and mod[0] not in seen: seen.add(mod[0]); print("SYSCALL (mmap/write) entered during", mod[0], "<-", " <- ".join(names[:6]))
PY
echo "--- -O0 of whole.bc linked with -fxray-instrument, XRAY_OPTIONS=patch_premain=true xray_mode=xray-basic"
timeout 100 gdb -q -batch -x xray_gdb.py ./t0 2>&1 | grep "^SYSCALL"; rm -f xr.*
