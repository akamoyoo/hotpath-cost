#!/usr/bin/env python3
"""Reads lines "<build> <op...> <ns>" from stdin and prints the median per build and operation."""
import collections, statistics, sys
t = collections.defaultdict(list)
for l in sys.stdin:
    b, *op, ns = l.split(); t[(b, " ".join(op))].append(float(ns))
ops = ["write_new_order", "read_all", "check"]
print(f"{'build':24}" + "".join(f"{o:>17}" for o in ops))
for cc in ["gcc13", "gcc15", "clang21"]:
    for o in ["O2", "O3"]:
        for v in ["orig", "inline", "fast", "both"]:
            b = f"b-{cc}-{o}-{v}"
            print(f"{cc + ' -' + o + ' ' + v:24}" + "".join(f"{statistics.median(t[(b, op)]):>17.0f}" for op in ops))
