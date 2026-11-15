#!/bin/bash
# Runs every attempt (build, analyse, run-time evidence) and leaves <attempt>/out.txt. FX_TASKSET="taskset -c ..." pins the runs.
cd "$(dirname "$0")"
for d in a* b* c* d* e* v*; do
  [ -x $d/run.sh ] || continue
  echo "=== $d"
  (timeout 900 $d/run.sh > $d/out.txt 2>&1; echo "rc=$?" >> $d/out.txt)
  grep -c "VIOLATION" $d/out.txt | sed 's/^/   VIOLATION lines: /'
done
