# llvm-fn-effects

**For every function of a C++ program, at compile time and without annotations: does it allocate, lock,
enter the kernel, throw or abort, counting everything it calls?**

`fn-effects` is a static analyser built on LLVM. It reads the whole program as LLVM bitcode, infers for
each function an *effect signature* that the source never declared, and says what each line of that
signature rests on. It is an independent project and is not affiliated with the LLVM project.

Status: first release, research quality. Linux x86-64, Clang/LLVM 21. Read "What is not claimed" before
relying on it.

## The problem

In systems with hard performance and reliability requirements (market-data handlers, order matching,
audio, control loops), Rust guarantees memory safety at compile time through ownership and borrow
checking. That removes one class of failure. Others remain, in Rust and more so in C++:

| Problem | What the language gives you | Here |
|---|---|---|
| Crashes in production | Out-of-range indices, overflow and failed assertions are found at run time | not addressed |
| Paying at run time for safety | Checks stay in hot loops unless removed by hand | not addressed |
| **Hidden costs on the hot path** | **A signature does not say whether a function allocates, locks or blocks. A dependency upgrade or a refactoring three calls down can add an allocation and nothing fails to compile** | **this tool** |
| Results that cannot be reproduced | Floating-point and optimisation differences between builds | a lint only (see `docs/RULES.md`) |

The third row is the one teams work around by convention: "no allocation after start-up", "this
function is wait-free", "`try_enqueue` never allocates". Such sentences live in comments and READMEs.
Nothing checks them, and they go stale silently.

The idea borrowed from Rust is the shape of the answer, not the mechanism: a property of the code is
established at compile time, it holds under a short fixed list of assumptions (Rust's is "no `unsafe`"),
and every place where the answer depends on an assumption is listed. `fn-effects` applies that shape to
effects, for C++ code that already exists, without asking anyone to rewrite or annotate it.

## What it prints

`examples/quick.cpp` has five small functions and no annotations:

```cpp
long checksum(const Order &o) { return o.id * 31 + o.px * 7 + o.qty; }
void append(const Order &o)   { book.push_back(o); }           // allocates when the vector grows
Order *make(long id)          { return new Order{id, 0, 0}; }  // allocates on every call
long insert_locked(const Order &o) { std::lock_guard<std::mutex> g(mu); append(o); return checksum(o); }
long route(const Order &o, bool slow) { return slow ? insert_locked(o) : checksum(o); }
```

```
$ fn-effects examples/quick.cpp --functions --class alloc --class lock
append
    alloc    may                library-policy   own account
    lock     may                library-policy   inside library functions
checksum
    alloc    never              own
    lock     never              own
insert_locked
    alloc    may                library-policy   own account
    lock     always             library-policy   own account
make
    alloc    always (elidable)  library-policy   own account
    lock     may                library-summary  consequence of alloc
route
    alloc    may                library-policy   own account
    lock     may                library-policy   own account

$ fn-effects examples/quick.cpp --why 'route(' --class alloc --class lock
== route(Order const&, bool)
  alloc: may, evidence library-policy, own account
      callees that bring it, direct calls (1):
         insert_locked [may, own]
  lock: may, evidence library-policy, own account
      callees that bring it, direct calls (1):
         insert_locked [always, own]
```

Each line has three parts.

**The verdict** says what can happen:

| verdict | meaning |
|---|---|
| `never` | no path of the call graph from this function reaches an operation of the class |
| `may` | some path does. It does not say the path is ever taken |
| `always` | every execution of the function that returns normally has called a primitive of the class (`malloc`, `operator new`, `pthread_mutex_lock`, a system-call wrapper, ...). `(elidable)` marks claims the optimiser may still remove at link time, such as an unused `new` |
| `unknown` | the analysis could not decide, and the report names the call that stopped it |

**The evidence** says what the verdict rests on, as the weakest link used. It decides whether a `never`
or an `always` is a guarantee or only the best available answer:

| evidence | rests on | guarantee? |
|---|---|---|
| `own` | the function's own instructions | yes |
| `direct` | plus direct calls inside the program | yes |
| `indirect-pta` | plus indirect calls resolved by points-to analysis | yes |
| `indirect-vtable` | plus virtual calls on a class hierarchy the build declares closed | yes |
| `indirect-type` | plus function pointers matched by type | yes, given assumption 3 below |
| `library-policy` | what the policy file says a library function does | no: trusted data |
| `library-summary` | a summary of a library binary | no: trusted data |
| `model` | a hand-written model of how a library function treats pointers | no |
| `closed-world` | that no other class, symbol or module replaces what was seen | no |
| `unknown` | nothing | no |

**The account** says whose fault a `may` is: the function reaches a primitive of that class (`own
account`), only a library function that does something inside (`inside library functions`), or only a
side effect of another class's primitive, such as the lock inside `malloc` (`consequence of alloc`).
An allocation is counted once, as an allocation.

The classes are data. `policies/hotpath.json` defines the ones above plus atomics, dynamic TLS,
recursion and others; `policies/noexcept.json` and `policies/signal-safety.json` ask two other questions
with the same engine.

## Three real cases

All three are reproduced by `cases/fetch.sh` and `python3 cases/run.py`, which fetch the projects at
pinned commits, compile a small driver with each to bitcode, and analyse it. Nothing of the projects is
executed. The quoted results are from this tree (`cases/out/<case>.why.txt`), with template arguments
shortened and classes that add nothing left out.

**1. A documented "never allocates" that allocates and takes a process-wide mutex.**
[moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) v1.0.4, `README.md:277`:
"`try_enqueue`, unlike just plain `enqueue`, will never allocate memory."

```
== fx_entry::cq_try_enqueue(ConcurrentQueue<Msg>&, Msg const&)
  alloc: may, evidence library-policy, own account
      callees that bring it, direct calls (1):  ConcurrentQueue<..>::get_or_add_implicit_producer [may, own]
  lock: may, evidence library-policy, own account
      callees that bring it, direct calls (1):  ConcurrentQueue<..>::get_or_add_implicit_producer [may, own]

== fx_entry::cq_try_enqueue_tok(ConcurrentQueue<Msg>&, ProducerToken&, Msg const&)
  alloc: never, evidence direct
  lock: never, evidence direct
  syscall: never, evidence direct
```

Without a producer token, the first `try_enqueue` of each thread registers an implicit producer: the
entry report points at `malloc` at `concurrentqueue.h:3466` and at `pthread_mutex_lock` inlined at
`:3504` (a `std::mutex` shared by the process). With a token, `never` holds with evidence `direct`: a
guarantee. The maintainer has confirmed the allocation in the project's issue 420 ("I believe this is
the only place `try_enqueue` may allocate"); the header comment of one overload mentions the one-time
producer, the README sentence does not, and neither mentions the mutex. This is not a bug report: the
library behaves as its author intends. It is the kind of sentence the tool checks.

**2. An allocation on every order-book operation.**
[Liquibook](https://github.com/enewhuis/liquibook) 2.0.0, `order_book.h:1048-1050`:
`Callbacks working; working.reserve(callbacks_.capacity()); working.swap(callbacks_);`

```
== liquibook::book::OrderBook<SimpleOrder*>::callback_now()
  alloc: may, evidence library-policy, own account
      own sites (a primitive, a symbol the policy names, an instruction): 2
         order_book.h:1049  operator new  [likely]
```

Every add, cancel and replace goes through this function. The project fixed it one commit after the
2.0.0 tag ("avoid reallocating callbacks vector"). The verdict is `may`, not `always`: proving that the
loop body runs needs a fact about a value (the callback list is not empty), which this analysis does
not track.

**3. A claim that holds.** [tcpshm](https://github.com/MengRao/tcpshm) says "Non-blocking", "No C++
exceptions", "No use of mutex or atomic operations". For the shared-memory poll path:

```
== fx_entry::tcpshm_poll_shm(Client&)
  alloc: never, evidence own      lock: never, evidence own      throw: never, evidence own
  free: never, evidence own       syscall: never, evidence own   abort: never, evidence own
```

Evidence `own`: the strongest statement the tool makes, resting on nothing but the function's
instructions. The TCP poll path of the same client is `may` for alloc and syscall, as expected.

On a larger program (QuickFIX, 849 functions in namespace `FIX`, measured during development and not
reproduced by the scripts here) the table reads: alloc `always` 31, `may` 691, `never` 127. Only 3 of
the 127 are guarantees; the other 124 have evidence `closed-world`. The per-function view is still
useful there: `Session::sendRaw` allocates because of 6 of its 102 callees, and the tool names them.
But see the limits below: on large programs most `never` answers are not yet guarantees.

## The assumptions

A verdict with guarantee-level evidence holds under these five assumptions. They are printed with every
report, together with each place where the answer used one.

1. The program has no undefined behaviour on the paths considered.
2. All code that can run is in the analysed program and stays as loaded: no `dlopen`, no JIT, no
   patching of loaded code, no symbol replaced by preloading beyond what was declared. The tool reports
   every `dlopen`/`dlsym` call and every replaceable symbol an answer depends on.
3. Indirect calls go where the resolution rule says. For virtual calls and typed function pointers this
   follows from assumption 1 when the whole program is visible; every resolved and unresolved indirect
   call is counted in the report.
4. Code the tool cannot read is listed, not assumed harmless.
5. The claim is about the function's own call tree. Signal handlers, other threads and the kernel
   (page faults, preemption) are outside it.

Trusted and not proved: the tool itself and the LLVM libraries it is built on. The arguments for each
rule are written out in [`docs/RULES.md`](docs/RULES.md) so that a reader can attack them, including
the definition of `always`, a theorem and its proof sketch.

## How it compares

| | When | Needs from you | Covers | What it does better than this tool |
|---|---|---|---|---|
| Profilers, heaptrack, allocation counters in tests | run time | a workload | paths the run took | measures how often and how much; no false positives |
| RealtimeSanitizer (`-fsanitize=realtime`) | run time | `[[clang::nonblocking]]` on entries, and a run that reaches the path | what executed, through libraries too | exact for what ran; maintained in Clang |
| Clang function effect analysis (`[[clang::nonallocating]]`, `-Wfunction-effects`) | compile time, per translation unit | the attribute on every function whose body the compiler cannot see; virtual calls are not inferred | annotated code | part of the compiler and the type system; diagnostics while you edit; enforced on every build |
| `noexcept` | run time | the keyword | exceptions only | a language rule: a violation terminates deterministically |
| stoat (LLVM IR call-graph checker for real-time code) | compile time | annotated roots | call graph | prior art for the approach; unmaintained since 2022 |
| Rust | compile time | writing Rust | memory safety | a guarantee that is part of the language. Effects are not in Rust signatures either |
| **fn-effects** | compile time, whole program | a Clang build of the program (or `compile_commands.json`) | every function, executed or not, with the callees responsible | |

What `fn-effects` adds: no annotations, the whole program across translation units, indirect and
virtual calls resolved with the rule used stated per answer, and a separation of guarantees from
answers that rest on trusted data. What it lacks: frequency (it never says how often), anything about
values, and the convenience of living inside the compiler. Its suite compares it against
RealtimeSanitizer on every test program: no event RealtimeSanitizer reports may be absent from the
static answer.

## Build and use

Needs LLVM 21 (headers, `clang++`, `opt`, `llvm-link`, `lld`), CMake, Ninja and Python 3.

```
cmake -S . -B build -G Ninja -DLLVM_DIR=<llvm>/lib/cmake/llvm -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++
ninja -C build
FX_LLVM_BIN=<llvm>/bin python3 tests/smoke.py
```

```
fn-effects a.cpp b.cpp --functions                         # sources: compiled -O2 -g -flto
fn-effects -p build/compile_commands.json --functions --prefix 'myns::'    # an existing project, build untouched
fn-effects whole.bc --why 'Session::sendRaw'               # bitcode you built yourself; one function explained
fn-effects ... --effects effects.json                      # the whole table as JSON
fn-effects ... -e 'Book::add' --policy policies/hotpath.json     # report per named entry
fn-effects ... --write-baseline accepted.json              # accept today's findings
fn-effects ... --baseline accepted.json --check            # CI: exit 1 only for what is new and forbidden
fn-effects --diff old.json new.json                        # exit 1 if a hazard appeared
```

Using it on an existing code base needs no source change. `-p` re-runs the project's own compile
commands with Clang to produce bitcode (for a GCC-built project it uses that GCC's headers and
libstdc++), so the project has to compile with Clang 21. The baseline mode is meant for adoption: accept
what is there today, fail the build only when a function that was clean stops being clean.

Library summaries (`summaries/*.json.gz`) describe what functions of glibc and libstdc++ do inside. A
summary is used only when the library on your machine is the build it was made from (Ubuntu 24.04:
glibc 2.39, the system libstdc++). On any other system the summaries are skipped and the answers that
would need them become weaker or `unknown`, never stronger.

## How it was tested

- `python3 tests/run_tests.py`: 44 cases with expectations written before the first run, plus 23
  regression cases from the last adversary round. Last run on the development machine (Ubuntu 24.04,
  LLVM 21.1.8): 1180 checks passed, 0 failed.
- Against RealtimeSanitizer on every test program: 78 run-time events, 0 that the static answer missed.
- Every `always` and every `never` with evidence `own` or `direct` is checked at run time under gdb with
  breakpoints on the primitives, in `-O0` and `-O2 -flto` builds of the analysed bitcode: 0
  counterexamples. Functions the test programs never call are counted as not run (for example 140 of
  194 functions ran in the `-O0` module build).
- Adversary rounds. After each development round a separate reviewer had one task: make the tool print a
  wrong `never` or `always` on a program with no undefined behaviour. The LLVM-level analyser was
  broken 8 times in the first such round, 3 times in the second, and 12 times when the per-function
  signatures were added. Each break became a rule and a regression case, and several rules were
  replaced by more conservative ones (a virtual call is now `closed-world` unless the build declares
  the hierarchy closed; inline assembly is `unknown` unless it is one of ten known harmless strings).
  There has been no adversary round on the final rules, so expect further breaks.

The full suite needs gdb, compiler-rt (for RealtimeSanitizer) and the library builds the summaries were
made from, so outside Ubuntu 24.04 some expectations will differ. CI in this repository only builds the
plugin and runs the smoke test.

## What is not claimed

- **Two known unsound cases are open.** Linker options that rename symbols (`--wrap`, `--defsym`) are
  not seen. A prebuilt object file linked into the program whose static constructor writes a function
  pointer into a global of the analysed code is not seen. Both violate "all code is in the analysed
  program", and the tool does not yet detect and report them.
- **On large programs most `never` answers are not guarantees.** The points-to analysis does not finish
  within its time budget on programs of a few thousand functions with heavy use of containers and
  type-erased callbacks; the tool then falls back to weaker rules and says so in the evidence column. In
  QuickFIX, 35,406 of 42,042 call edges come from matching function pointers by type, most of them
  through one settable deallocation hook in a bundled XML parser.
- **`may` is reachability.** It over-approximates. It never says how often, and it reports paths that
  cannot happen for reasons that depend on values.
- **The analysis is of pre-link bitcode.** The shipped binary can do less (the optimiser removes
  calls), and what the dynamic loader does (lazy binding) is outside. A companion analyser for ELF
  binaries exists as a prototype and is not released; here it only produced the library summaries.
- **Library data is trusted, not proved.** Policy files and summaries are the weakest link of every
  answer that uses them, which is why they are their own evidence levels.
- Linux x86-64 and LLVM 21 only. C++ exceptions are not matched against handlers. No Windows, no
  macOS, no ARM.
- No user outside the author has run it on a production code base.

## Layout

```
fn-effects        driver (Python): builds bitcode, runs the plugin, prints reports
src/              the LLVM pass plugin: call graph, points-to analysis, signatures
policies/         hazard classes as data
summaries/        library summaries for glibc 2.39 and libstdc++ (Ubuntu 24.04)
cases/            the three real cases: fetch, drivers, run
tests/            cases, adversary regressions, run-time validators
docs/RULES.md     every rule and the argument for it
```

## History

This repository earlier held design notes for a contract-checked language called Taut. That work is
shelved. The question it started from, what a function can do beyond what its signature says, is the
one this tool answers for C++ that already exists.

## License

Apache-2.0.
