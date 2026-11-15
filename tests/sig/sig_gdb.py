# gdb script for check.py. FX_CLAIMS {func: {class: "always"|"never:<ev>"}}, FX_PRIMS {class: [symbols]}, FX_ADDRS {func: addr}
import gdb, json, os
CL = json.loads(os.environ["FX_CLAIMS"]); PRIMS = json.loads(os.environ["FX_PRIMS"]); f_addr = json.loads(os.environ["FX_ADDRS"])
gdb.execute("set pagination off"); gdb.execute("set confirm off"); gdb.execute("set breakpoint pending on")
active = []      # [function, {class: first primitive symbol}, thread]
stats = {}       # (f, class) -> [normal returns, returns with event, unwound/abandoned]
viol = set()
CAP = 300
class Prim(gdb.Breakpoint):
    def __init__(self, spec, cls):
        # -qualified: `_Znwm` must not also match Class::operator new(unsigned long) (gdb matches names in every scope by default)
        super().__init__(spec if spec.startswith('*') else '-qualified ' + spec, internal=True); self.cls = cls; self.sym = spec
    def stop(self):
        if active:
            t = gdb.selected_thread().num
            for a in active:
                if a[2] == t: a[1].setdefault(self.cls, self.sym)
        return False
def settle(act, normal):
    f, seen, _ = act
    if act in active: active.remove(act)
    for c, k in CL[f].items():
        st = stats.setdefault((f, c), [0, 0, 0])
        if not normal: st[2] += 1
        else: st[0] += 1
        if c in seen: st[1] += 1 if normal else 0
        if k == 'always' and normal and c not in seen and (f, c) not in viol:
            viol.add((f, c)); print(f"VIOLATION always {c}: {f} returned normally and no primitive of {c} was called"); gdb.execute("bt 4")
        if k.startswith('never') and c in seen and (f, c) not in viol:
            viol.add((f, c)); print(f"VIOLATION {k} {c}: during {f} the primitive {seen[c]} was called ({'normal return' if normal else 'left by unwinding'})")
class Done(gdb.FinishBreakpoint):
    def __init__(self, frame, act):
        super().__init__(frame, internal=True); self.act = act
        self.sp = int(gdb.parse_and_eval("$sp")); self.ra = int(gdb.parse_and_eval("*(unsigned long*)$sp"))
    def stop(self):
        # a normal return: at the return address with the caller's stack pointer (a longjmp/setcontext that lands elsewhere is not one)
        pc = int(gdb.parse_and_eval("$pc")); sp = int(gdb.parse_and_eval("$sp"))
        
        if os.environ.get('FX_DEBUG'): print('DBG', self.act[0], hex(pc), hex(self.ra), hex(sp), hex(self.sp))
        # ... reached by a `ret` instruction of the program (the last one this thread executed popped this very slot)
        lr = lastret.get(gdb.selected_thread().num)
        settle(self.act, pc == self.ra and sp == self.sp + 8 and lr == (self.sp, self.ra)); return False
    def out_of_scope(self):
        settle(self.act, False)
lastret = {}
class RetI(gdb.Breakpoint):
    def stop(self):
        sp = int(gdb.parse_and_eval("$sp"))
        lastret[gdb.selected_thread().num] = (sp, int(gdb.parse_and_eval("*(unsigned long*)$sp")))
        return False
class Entry(gdb.Breakpoint):
    def __init__(self, f):
        super().__init__("*" + f_addr[f], internal=True); self.f = f; self.n = 0
    def stop(self):
        self.n += 1
        if self.n > CAP: return False
        act = [self.f, {}, gdb.selected_thread().num]
        active.append(act)
        try: Done(gdb.newest_frame(), act)
        except Exception as e: active.remove(act)
        return False
if os.environ.get("FX_SOLIB"):
    gdb.execute("tbreak dlsym"); gdb.execute("run")
    an = os.environ["FX_ANCHOR"]
    base = int(gdb.parse_and_eval("(unsigned long)&'%s'" % an)) - int(f_addr[an], 16)
    f_addr = {k: hex(int(v, 16) + base) for k, v in f_addr.items()}
    os.environ["FX_RETS"] = json.dumps([hex(int(a, 16) + base)[2:] for a in json.loads(os.environ.get("FX_RETS", "[]"))])
else:
    gdb.execute("break main"); gdb.execute("run")
for f in CL: Entry(f)
for a in json.loads(os.environ.get('FX_RETS', '[]')): RetI('*0x' + a, internal=True)
import re as _re
def lib_addr(sym):   # the address of sym inside a shared library (not the program's own definition, not its PLT stub)
    out = gdb.execute("info address " + sym, to_string=True)
    best = None
    try:
        o = gdb.execute("info functions -n ^" + _re.escape(sym) + "$", to_string=True)
    except gdb.error: o = ''
    for ob in gdb.objfiles():
        if ob.filename and ('.so' in ob.filename):
            try:
                s = ob.lookup_global_symbol(sym) or ob.lookup_static_symbol(sym)
            except Exception: s = None
            if s is not None: return int(s.value().address)
            ms = gdb.execute("info symbol 0", to_string=True) if False else None
    return None
for c, names in PRIMS.items():
    for n in names:
        if n.startswith('*LIB*'):
            sym = n[5:]
            done = False
            for ob in gdb.objfiles():
                if ob.filename and '.so' in ob.filename:
                    try:
                        o = gdb.execute(f"info address {sym}", to_string=True)
                    except gdb.error: o = ''
                    break
            # minimal symbols of libraries: ask gdb for every location of the name and keep those outside the program
            try:
                locs = gdb.decode_line(sym)[1] or []
            except gdb.error: locs = []
            for sal in locs:
                so = gdb.solib_name(sal.pc)
                if so: Prim("*%#x" % sal.pc, c); done = True
        else:
            Prim(n, c)
try: gdb.execute("continue")
except gdb.error as e: print("PROGRAM", e)
for (f, c), (n, ok, uw) in sorted(stats.items()):
    print(f"RT {f} {CL[f][c]} {c}: normal-returns {n}, with-primitive {ok}, not-returned {uw}")
for f in CL:
    if not any(k[0] == f for k in stats): print("NOTRUN", f)
print("RT-END")
