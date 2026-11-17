#!/usr/bin/env python3
"""Analyse the three cases of cases/README.md and write cases/out/<case>.{txt,json,effects.json,why.txt}.

  cases/fetch.sh
  python3 cases/run.py [case ...] [-- extra fn-effects options, e.g. --gcc-toolchain /usr]

Each case is a small driver (cases/drivers/<case>/main.cpp) whose entry functions call one documented
operation of the project with opaque arguments. The driver is compiled with the project to whole-program
bitcode and analysed. Nothing is executed.
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), 'fn-effects')
S, D, OUT = os.path.join(HERE, 'src'), os.path.join(HERE, 'drivers'), os.path.join(HERE, 'out')
BASE = '-O2 -DNDEBUG -pthread -Dnoipa=noinline'   # noipa is GCC's attribute; Clang's nearest is noinline
CASES = {
    # Clang reports __GNUC__ 4.2, for which the header turns its thread_local support off; the define gives
    # the configuration every GCC build of the header has.
    'concurrentqueue': dict(
        srcs=[f'{D}/concurrentqueue/main.cpp'], std='c++17',
        flags=f'-I{S}/concurrentqueue -DMOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED',
        why=['fx_entry::cq_try_enqueue(', 'fx_entry::cq_try_enqueue_tok(', 'fx_entry::cq_try_dequeue(']),
    'liquibook': dict(
        srcs=[f'{D}/liquibook/main.cpp', f'{S}/liquibook/src/simple/simple_order.cpp'], std='c++17',
        flags=f'-I{S}/liquibook/src',
        why=['fx_entry::lb_cancel(', '::callback_now(']),
    'tcpshm': dict(
        srcs=[f'{D}/tcpshm/main.cpp'], std='c++11', flags='',
        why=['fx_entry::tcpshm_poll_shm(', 'fx_entry::tcpshm_poll_tcp(']),
}


def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        i = argv.index('--'); argv, extra = argv[:i], argv[i + 1:]
    os.makedirs(OUT, exist_ok=True)
    rc = 0
    for name in argv or CASES:
        c = CASES[name]
        o = os.path.join(OUT, name)
        cmd = [sys.executable, TOOL] + c['srcs'] + ['--std=' + c['std'], '--cflags=' + (BASE + ' ' + c['flags']).strip(),
               '-e', 'fx_entry::*', '-o', o + '.json', '--text', o + '.txt', '--effects', o + '.effects.json',
               '--path-base', HERE, '--pta-budget', '120'] + extra
        p = subprocess.run(cmd, capture_output=True, text=True)
        print(name, 'ok' if p.returncode == 0 else 'FAILED')
        if p.returncode != 0:
            print(p.stderr[-2000:]); rc = 1
            continue
        with open(o + '.why.txt', 'w') as f:
            for w in c['why']:
                q = subprocess.run([sys.executable, TOOL, '--from-effects', o + '.effects.json', '--why', w],
                                   capture_output=True, text=True)
                f.write(q.stdout + q.stderr + '\n')
    return rc


if __name__ == '__main__':
    sys.exit(main())
