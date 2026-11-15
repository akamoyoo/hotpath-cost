import gdb
gdb.execute("set pagination off"); gdb.execute("handle SIGTRAP nostop noprint pass"); gdb.execute("handle SIGUSR1 nostop noprint pass")
gdb.execute("break main"); gdb.execute("run"); gdb.execute("break -qualified malloc")
for _ in range(6):
    try: gdb.execute("continue", to_string=True)
    except gdb.error: break
    try: f = gdb.selected_frame()
    except gdb.error: break
    names = []
    while f is not None and len(names) < 8: names.append(f.name() or "?"); f = f.older()
    if "on_trap" in names: print("MALLOC called under:", " <- ".join(names))
