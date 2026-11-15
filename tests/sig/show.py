import json, sys
E = json.load(open(sys.argv[1]))['functions']
C = ['alloc', 'free', 'lock', 'syscall', 'throw']
for pat in sys.argv[2:]:
    for f, v in E.items():
        if pat in f or pat in v.get('demangled', ''):
            s = v.get('signature', {})
            out = []
            for c in C:
                x = s.get(c, {})
                t = f"{c}={x.get('verdict')}{'(elidable)' if x.get('qualifier') == 'elidable' else ''}[{x.get('evidence')}"
                if x.get('account'): t += f",{x.get('account')}"
                if x.get('verdict') == 'always': t += ",rests_on=" + "+".join(x.get('always', {}).get('rests_on', []))
                out.append(t + "]")
            print("SIG", f, " ".join(out))
