import gdb
gdb.execute("set pagination off")
gdb.execute("catch syscall getpid")
gdb.execute("run")
seen = []
for _ in range(40):
    try:
        f = gdb.selected_frame()
    except gdb.error:
        break
    n = f.name()
    if n and n not in seen:
        seen.append(n); print("SYSCALL getpid entered from", n, "(caller", f.older().name(), ")")
    try:
        gdb.execute("continue")
    except gdb.error:
        break
