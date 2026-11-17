# Sourced by each attempt's run.sh (cwd = the attempt directory).
#   analyse "SRCS..." [extra fn-effects options]   -> w/whole.bc (the module fn-effects analysed), effects.json, report.json
#   show FUNC...                              -> verdict/evidence lines of the named functions (substring of symbol)
#   build0 / build2 [link flags]              -> ./t0: clang++ -O0 -g -no-pie w/whole.bc (exactly the tool's own check)
#                                                ./t2: clang++ -O2 -flto -fuse-ld=lld -no-pie w/whole.bc (the LTO link)
#   check EXE [FUNC...]                       -> gdb run: every always / never(own|direct) claim against primitive breakpoints
HP=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SIG=$HP/tests/sig
LLVM=${FX_LLVM_BIN:?set FX_LLVM_BIN to the bin directory of LLVM 21}
export PATH=$LLVM:$PATH
TS=${FX_TASKSET:-}
CXX=$LLVM/clang++
analyse() {
    local srcs=$1; shift
    rm -rf w effects.json report.json
    $TS python3 $HP/fn-effects $srcs --work w -o report.json --text report.txt --effects effects.json "$@" > fn-effects.log 2>&1 || { echo "fn-effects FAILED"; tail -5 fn-effects.log; }
}
show() { python3 $SIG/show.py effects.json "$@"; }
build0() { $TS $CXX -O0 -g -no-pie -fno-pie w/whole.bc -o t0 -pthread -w "$@"; }
build2() { $TS $CXX -O2 -g -flto -fuse-ld=lld -no-pie w/whole.bc -o t2 -pthread -w "$@"; }
check2() { local exe=$1; shift; FX_BUILD=O2 $TS python3 $SIG/check.py $exe effects.json "$@"; }   # the -O2 -flto build: always (elidable) is not claimed there
check() { local exe=$1; shift; $TS python3 $SIG/check.py $exe effects.json "$@"; }
