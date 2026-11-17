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
