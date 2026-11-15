#!/bin/bash
# Table-level check of `never` with evidence own/direct on every effect table at hand: the tool's test outputs and this directory's attempts.
cd "$(dirname "$0")"; . ../common.sh
python3 evcheck.py $HP/tests/out/*/effects.json ../*/effects*.json > out.txt 2>&1; tail -1 out.txt; grep -c MISLABEL out.txt; grep MISLABEL out.txt | head -12; grep NOTE out.txt | head -12
