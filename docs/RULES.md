# fn-effects: the rules, and why each one holds

This is the reference for what the analyser computes: every rule, with the argument for it. The
README is the introduction. "Round N" names a development round: after each one an adversary (a separate
reviewer whose only task was to make the tool print a wrong `never`, `none` or `always`) attacked the
rules, and the breaks became rules and regression cases. "The binary analyser" is an unreleased
companion that reads ELF files; here it only supplies the library summaries in `summaries/`.

`fn-effects` answers, for named entry functions of a C++ program and everything they can call, which classes
of behaviour (allocation, locks, system calls, throw, abort and trap sites, atomics, dynamic TLS,
recursion, unresolved indirect calls, ...) are reachable, with a call chain, a source location (with the
inlining chain) and a hot/cold tag, and which are not reachable under which printed assumptions. No
annotations in the code. It also writes the inferred effect set of every function in the program (the
effect signature the code never declared); a per-entry report is a query over that table.

The classes are data: a policy file defines them. `policies/hotpath.json` is the hot-path policy of
the README; `policies/noexcept.json` and `policies/signal-safety.json` are two other policies on the
same engine. `policies/common.json` holds ABI and compiler facts shared by all policies.

## Build and run

Needs an LLVM 21 installation (headers, `clang++`, `opt`, `llvm-link`, `llvm-nm`, `llvm-cxxfilt`).

    cmake -S . -B build -G Ninja -DLLVM_DIR=<llvm>/lib/cmake/llvm -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++
    ninja -C build                             # builds build/FnEffects.so
    python3 tests/run_tests.py              # every test, including the RealtimeSanitizer comparison

The driver finds the LLVM tools through `--llvm-bin`, `FX_LLVM_BIN`, the LLVM the plugin was built
against, or `llvm-config` on `PATH`.

    fn-effects a.cpp b.cpp -e 'Book::add' -e 'Book::*'          # sources: compiled -O2 -g -flto -fwhole-program-vtables
    fn-effects whole.bc --entries-file hot.txt                 # bitcode you built yourself
    fn-effects -p build/compile_commands.json --entries-file hot.txt --cc-exclude test/   # an existing project
    fn-effects ... --policy policies/noexcept.json             # another question, same engine
    fn-effects ... --effects effects.json                      # effect table of every function
    fn-effects ... --functions --prefix 'FIX::'                # per function: one line per class, verdict and evidence
    fn-effects ... --why 'FIX::Session::sendRaw'               # one function: own sites, and the callees that bring each class
    fn-effects --from-effects effects.json --why NAME          # the same on an existing table
    fn-effects ... --pta-scope component                       # points-to per component (large programs)
    fn-effects ... --write-baseline accepted.json              # accept today's findings
    fn-effects ... --baseline accepted.json --check            # CI: exit 1 only for what is new and forbidden
    fn-effects --diff old.json new.json                        # exit 1 if a hazard appeared

Outputs: the JSON report (`-o`, default `fn-effects-report.json`), a text report (`--text`, default stdout: per
entry one verdict line per class, then findings, hot first, then what the verdicts rest on, then the
blind spots), and optionally the effect table (`--effects`).

Entries: `-e PATTERN` or `--entries-file` (one pattern per line), matched against the mangled name, the
demangled name, and the demangled name without parameters; `*`, `?` and `[...]` make a glob. A policy can
also select entries: `annotate:hotpath` (`__attribute__((annotate("hotpath")))`), `attr:<attribute>`,
`has-terminate-pad`, `registered-with:signal|sigaction`, `name:<pattern>`, `defined`, combined with `&`
and `!`.

## What a verdict means

For class C and entry E the verdict is one of (the same four symbols as the binary analyser):

* `none`: no operation of class C is reachable from E in the call graph built by the rules below, no
  site on the way blocks the answer, and the answer rests only on the module's own IR and on rules that
  need no library data. This is the guarantee, under the assumptions printed with the report
  (`global_assumptions`).
* `none*`: the same, except that the answer rests on library data: a library summary, a `known` or
  policy pattern that says an external function does nothing of a class, a pointer model weaker than the
  default, or a closed-world rule (rapid type analysis, the closed-hierarchy rule). It is **not a
  guarantee**. The entry lists what it trusts (`trusted`: `summary:<sym>`, `known:<sym>`, `policy:<sym>`,
  `pointer-model:<sym>`, `closed-world:<n> indirect calls`, `interposable:<def>`). `--strict` turns every
  `none*` into `unknown`. `--check` treats `none*` like `unknown`: a forbidden class that is `none*` fails
  the check unless the baseline accepts it (`<entry>|<class>|verdict:none*`).
* `reachable`: a path exists; the report gives a witness chain. It does not say the path is taken.
* `unknown`: something on the way could not be analysed: an external function with no body, no policy
  entry and no summary; an indirect call with no target or an open target set; inline assembly that can
  transfer control; an intrinsic the engine does not know; a summary that has unresolved indirect calls;
  a translation unit that did not compile. Each such site is a finding of class `external-unknown` or
  `indirect-unknown`, so the report says which site stopped it. `unknown` is never turned into `none`.

The five assumptions listed in the README are the frame; `policies/common.json` prints them in
the form this analyser relies on. Each place a verdict rests on one is listed in the entry's report:
every indirect call with its rule and number of targets (`indirect_calls`, ids into the top-level
`indirect_calls` list), every unresolved or open one (findings `indirect-unknown`), every external with
no body or summary (`external-unknown`), every summary used, every `dlopen`/`dlsym` and every
`mmap`/`mprotect` that may request `PROT_EXEC` (`module.dynamic_code` and per-entry assumptions), every
inline assembly block (assumption `inline-asm` or finding), every external symbol whose policy entry a
`none` depends on (`relies_on_externals`: these are the symbols an `LD_PRELOAD` or a kernel-bypass
library could replace), and every reached definition that can be replaced at link or load time
(`interposable_definitions`: weak definitions, and exported functions of a `-fPIC` shared object).

## No default says "nothing happens" (round 5)

Every break an adversary found in round 5 came from data or a default that said "nothing happens"
without proof. The defaults are now the other way round. The rules, as the code applies them:

**R1. Provenance.** An integer keeps the provenance of a pointer only while it is a copy of one (`load`,
`store`, `ptrtoint`, `bitcast`, `phi`, `select`, `extract`/`insertvalue`, `freeze`). Every other integer
carries "no provenance" (the special object NOPROV): the result of any integer arithmetic (`add`, `mul`,
shifts, ...; the operands' provenance is kept as well, with the offset lost), a widening, narrowing or
floating-point conversion, an integer returned by a library function, an intrinsic's integer result, an
integer constant whose bits can be part of an address (non-zero, and for 8-byte values in
[65536, 2^57)), the bytes of a global's initializer of that kind, and bytes written by a library from
outside (R2). A store of a value whose type carries no provenance at all (`i32`, `i16`, floating point,
non-zero) writes NOPROV into the slot; a non-zero `memset` does too. As a pointer, NOPROV means exactly
what UNK means: a load through it gives UNK, a store through it goes into every exposed object, a call
through it falls back to the type rule for that call. A pointer computed from a function's address at any
offset other than 0 is UNK, and so is every `inttoptr` (it keeps the integer's provenance as well). So a
load that can see a slot written with an integer without provenance falls back for that call, whatever
the slot held before. Exposed objects are those whose address became an integer or escaped to library
code; a pointer of unknown provenance that reaches library code makes every exposed object escape.
The Steensgaard pre-pass ignores NOPROV: an object it proves can never hold a code pointer (DATA) cannot
lead to a call through anything loaded from it, forged or not, because any store of code through such a
pointer would have made the object relevant.
Three refinements keep this from drowning real programs, each sound on its own terms. (a) Strict
aliasing as LLVM's TBAA states it: bytes without provenance in memory carry the TBAA type of the store
that wrote them; a load drops those whose type its own type cannot alias (neither is an ancestor of the
other in the type tree; `vtable pointer` aliases nothing but itself), and every value read from memory
then holds only generic NOPROV. Untyped and union accesses (Clang gives them `char`), `memcpy`, and
loads without `!tbaa` filter nothing. This is assumption 1 (no undefined behaviour), the same rule the
compiler optimises by. (b) A constant-length copy of up to 512 bytes (`memcpy`, a struct assignment) goes
in 8-byte chunks, so an integer field and a pointer field of the source stay in separate slots. (c) The
container models (`data`: string members, red-black tree and list links) dereference only link
pointers: the integers in the same headers (a length, a node colour) are not read as pointers. A store
through a forged pointer never writes a constant global (vtables, typeinfo: undefined behaviour).
*Derived:* attack 08 (`anchor + rel[i]`): the sum has the anchor's provenance at an unknown offset: UNK,
fallback, the handlers are targets. Attack 12 (`memcpy(&hook, &v, 8)` with `v` parsed from text): `v` is
computed (NOPROV), the slot `hook` holds NOPROV, the call through it falls back.

**R2. Library pointer models.** A function without a body is assumed to escape its pointer arguments, to
write anything through them at any time, and to return a library pointer (EXT). An entry of
`abi.pointer_models` weakens this only with a one-line note citing the function's documented contract
(the loader refuses an entry without one). A function that writes bytes from outside (`read`, `recv`,
`recvfrom`, `pread`, `fread`, `fgets`, `epoll_wait`, `ioctl`, ...) or computed bytes (`snprintf`'s
output, `clock_gettime`'s `timespec`, a mutex's internals) is never `none` for those bytes: the
destination receives NOPROV (`writes_bytes`, with a size where the contract fixes one). A function that
sends bytes out (`write`, `send`, `fwrite`, `puts`) exposes the objects whose pointers those bytes hold
(`sends`): they may come back through a read. `printf` exposes its variadic pointers (`%p`) and writes
through them (`%n`) unless the format is a constant without those conversions. `bsearch` returns a
pointer into argument 1; `qsort` moves pointers within its array; `mmap` returns fresh memory with
NOPROV contents. The 115 entries are listed in `policies/common.json`, each with its note.

**R3. Trust marking.** `none` only when the verdict rests on the module's own IR and on rules needing no
library data; `none*` when it rests on a summary, a `known` or policy pattern, a pointer model or a
closed-world rule, with the list of trusted items (above). Pointer models count for an entry only when
one of its indirect calls was resolved by the points-to analysis *and* a second solution in which every
library function has the default model (and no body is replaced) does not bound that call's targets as
well (`module.points_to.resolutions_without_models` counts the calls it does bound). vDSO functions:
the fallback system call is counted only when the summary names it (`vdso.fallback_syscalls`); without
that field the verdict for `syscall` is `unknown`.

**R4. Calls the IR does not show.** An atomic operation whose alignment is below its size is a call to
libatomic's generic (lock-based) function, whatever its width; one wider than the lock-free width (8,
or 16 with `+cx16`) calls the sized function. Every intrinsic that stops the process (`llvm.trap`,
`llvm.ubsantrap`, `llvm.debugtrap`) is in the `abort` class. `abi.intrinsics_no_call` was reviewed entry
by entry against LLVM 21's x86-64 lowering (its note lists what was removed and why):
`instrprof.value.profile` calls `__llvm_profile_instrument_target`, `smul.with.overflow.i128` calls
`__muloti4`, `convert.to/from.fp16` call `__truncsfhf2`/`__extendhfsf2`, `llvm.x86.int` is a software
interrupt (`syscall`); `sdiv`/`udiv`/`srem`/`urem` wider than 64 bits and `fp128` arithmetic call the
compiler runtime (`__divti3`, `__addtf3`, ...).

**R5. Translation units that did not compile.** If any unit failed, rapid type analysis and the
closed-hierarchy rule are off and every vtable of the module counts as installed. A virtual call on an
object that may come from outside the module keeps every vtable target, plus the escaped functions the
points-to analysis gives (`vtable+ext(incomplete module)`), and is open if a missing unit defines a
vtable. The report says so (`module.incomplete_translation_units`, the text report's build line).

**R6. Tags.** A site's tag is `likely`, `unlikely`, `first-use` or `unknown` (section "Tags"). `unlikely`
means only "the compiler's branch hints (or a cold call) say so": such a site can be executed on every
run. `first-use` marks the initialisation of a function-local static or a thread-local. A tag never
changes a verdict, a violation or a `--check` result.

## Per-function signatures (round 6)

The product is a table: for every defined function and class, a verdict, the evidence it rests on, on
whose account it is, and the callees that bring it. A per-entry report is a query over it (the suite
checks, for every entry of every case, that the entry verdict and the function's row agree).
`fn-effects --effects FILE` writes the table (per function `own` sites, `calls`, `signature`, `returns`);
`fn-effects --functions [--prefix NS::]` prints it (three summary tables, then one line per function and
class); `fn-effects --why FUNCTION` prints one function's own sites and the callees that bring each class,
direct calls first; `--from-effects FILE` does both on an existing table. Purely static: no profile, no
annotation, no knowledge supplied by the user beyond the policy file.

It is computed bottom-up: the strongly connected components of the call graph in reverse topological
order (callees first), each function's row from its own body and its callees' rows, iterated to a fixed
point inside a component. The plugin checks that the `may` sets it gets this way are exactly those of
the effect table (`module.signature_mismatches`, an internal error if not 0).

**Verdicts.** For function f and class C:

* `always`: every execution of f that returns normally has called a primitive of C (or executed an
  instruction of C, for the instruction classes). Defined and proved below.
* `may`: some path of the call graph reaches an operation of C (the effect table's `reachable`). It does
  not say the path is taken.
* `never`: no path reaches one, and nothing on the way blocks the answer.
* `unknown`: no path found, but an unresolved or open indirect call, an unclassified external, or a
  summary that leaves C unknown is reached.

**Primitives.** A policy class may name `primitives`: library functions every call of which is an
operation of the class. In `policies/hotpath.json`: alloc: `malloc`, `calloc`, `realloc`, `aligned_alloc`,
`posix_memalign`, ..., every `operator new`, `__cxa_allocate_exception`, `mmap`, `brk`; free: `free`,
`operator delete`, `munmap`; lock: `pthread_mutex_lock/unlock/trylock`, the rwlock, spinlock and
semaphore operations; syscall: the system-call wrappers (`read`, `write`, `send`, `recv`, ...; the
list is in the policy); throw: `__cxa_throw`, `__cxa_rethrow`, `std::__throw_*`; abort: `abort`, `exit`,
`std::terminate`, the assertion and hardening handlers. "Called a primitive" means the call instruction
was executed: `malloc` may return null, `operator new` may throw, a wrapper may reject its arguments
before it enters the kernel (`sigaction` with an invalid signal number). That is what `always` claims,
no more.

**Normal return.** A return is a `ret` of f. A path that ends in `unreachable`, in `resume`, in a call
that never returns, or in an exception that propagates out of a `call`, is not a return. A landing pad
is an ordinary block: a caught exception that leads to a `ret` is a returning path.

**Definition of `always` (as computed).** Let R be the blocks of f from which a `ret` is reachable, and
Must the blocks that lie on every path from the entry to a `ret` inside R: the post-dominators of the
entry in the control-flow graph restricted to R (greatest fixed point of Must(B) = {B} ∪ ⋂ Must(S) over
the successors S of B in R; both edges of an `invoke` are successors). f is `always C` if one of these
holds (least fixed point over the call graph, component by component, so that a cycle cannot make
itself `always`):

1. a direct call of a primitive of C in a block of Must (the call is executed when its block is, even if
   it then throws: a block is left only through its terminator, or by a call that does not come back, and
   then f does not return);
2. an instruction of C (an atomic instruction, a dynamic `alloca`, ...) in a block of Must; not a `phi`
   (its operand belongs to one edge) and not an inline-assembly block (the matched instruction may sit
   behind a jump inside it);
3. a call in a block of Must, every target of which has a body in the module and is `always C`, where
   - the call is direct, or indirect and resolved by points-to without a fallback, without a library
     pointer in the callee set and without a pointer model (`indirect-pta`), by a constant table, or by
     the vtable rule's own target set on a hierarchy that is not open: evidence `indirect-vtable` if the
     hierarchy is established as closed, `closed-world` otherwise (rule (a) below). Never: type matching
     (it is the fallback rule: a pointer made by a library is not among its targets), an ifunc, an open
     or unresolved call, a call that may go into library code, any indirect call when a translation unit
     did not compile;
   - if the call is an `invoke`, its landing pad cannot reach a `ret` (otherwise the callee may have
     unwound, and its guarantee covers only its normal returns);
   - no target is an `available_externally` copy of a library's definition (the library's own code
     runs, compiled separately); a target that the linker or loader may replace (a weak definition, an
     exported function of a `-fPIC` shared object, a call through a weak or exported alias) is accepted
     with evidence `closed-world` (rule (c) below).

Nothing is claimed for f when: no `ret` is reachable from its entry (the statement would be empty); f
calls a `returns_twice` function (`setjmp`); f contains inline assembly that can transfer control
(`abi.asm_unknown`) or a `callbr`; f is a coroutine that is not yet split, a `naked` function, or an
`available_externally` copy. Library summaries never give `always` (they say "may"), and neither do the
callbacks a library might make.

How the cases the task names are treated: *invokes and landing pads* as above (a primitive called by
an `invoke` counts; a callee's `always` through an `invoke` counts only if the unwind edge cannot
return). *Loops that may not terminate* need no special rule: a block inside or after a loop is in Must
only if every path to a `ret` passes it, and an execution that never leaves the loop never returns, so
nothing is claimed about it. A loop that may run zero times has an edge around its body: the body is
not in Must. *`musttail` and `tail` calls* are calls: f returns normally only if the callee did, so
rule 3 applies unchanged. *A throw path* is not a return: `if (x < 0) throw ...; malloc(...)` is `always
alloc`.

**Theorem.** Under assumption 1 (no undefined behaviour), for the module as analysed: if f is
`always C`, every call of f that returns normally has called a primitive of C (or executed an
instruction of C) during that call. A claim whose justification uses an `indirect-vtable` call rests
in addition on the build's statement that the class has hidden LTO visibility (or on its internal
linkage); one with evidence `closed-world` needs that no interposable symbol on the way is bound to
another definition and that no class outside the module derives from the classes of the virtual calls
it uses (assumptions 2 and 3): it is not a guarantee. A primitive is a library function (rule (b)). `signature.<class>.always.rests_on` lists every evidence kind a claim uses.

*Proof sketch*, by induction on the step of the fixed-point iteration at which the claim was set. A
returning execution of f follows a path of the control-flow graph from the entry to a `ret`; every block
on it can reach a `ret`, so the path lies in R and passes every block of Must. A block that is passed
on a returning path is executed up to and including its terminator: the only ways to leave it earlier
are a call that does not return to f (it unwinds out of f, exits, or loops forever), and then f does not
return, or control flow the graph does not show (`setjmp`/`longjmp`, a jump out of inline assembly, a
coroutine suspend), which the "nothing is claimed" list excludes. Rules 1 and 2: the primitive call or
the instruction is therefore executed. Rule 3: the call instruction is executed. If it is a `call`, f
continues only if the callee returned normally (had it unwound, the exception would have left f). If it
is an `invoke`, the path continues either on the normal edge, and the callee returned normally, or on
the unwind edge, which by the condition cannot reach a `ret`, so this is not a returning execution.
The callee is one of the listed targets: for a direct call it is the constant callee, and its body is
the code that runs unless it is replaced (excluded, or flagged `closed-world`); for an indirect call
the rule's target set is complete (constant table: the loaded value is one of the initializer's, by
assumption 1; points-to without EXT, UNK or a fallback: the soundness argument of the points-to
section, with no pointer model involved; vtable on a closed hierarchy: the object model, assumption 3).
Every target was `always C` at an earlier step, so by the induction hypothesis the callee's returning
execution called a primitive of C. Recursion: the least fixed point only ever sets a claim from claims
set before it, so the induction is well founded.

What the theorem does not say. It is about the IR that was analysed (pre-link bitcode): the final link
may still remove an allocation whose result is unused (LLVM deletes `new`/`delete` pairs, C++14 allows
it), so the shipped binary can do less, never more of another class: claims that rest only on such
calls are marked `always (elidable)` (rule (f) below). It says "called", not "succeeded"
or "entered the kernel". It says nothing about executions that do not return, and nothing about how
often: one call suffices.

**Run-time check of the theorem**: rule (g) below (`tests/validate_always.py`, `tests/sig/check.py`).
Functions the test programs never call are counted as not run. Field programs are not run (static only).

**Evidence**, from strongest to weakest; a verdict carries the weakest link it rests on:

| evidence | what the verdict rests on |
|---|---|
| `own` | the function's own instructions only |
| `direct` | plus direct calls into functions of the module |
| `indirect-pta` | an indirect call resolved by points-to without pointer models, or from a constant table |
| `indirect-vtable` | the vtable rule (also when points-to fell back to it) on a hierarchy established as closed: internal linkage or hidden LTO visibility (rule (a)) |
| `indirect-type` | type matching of address-taken functions, any other fallback, a pointer that may come from a library and is assumed to stay in the module |
| `library-policy` | a primitive or another symbol the policy names (also `known`), a callback a library may make |
| `library-summary` | what a library summary says a library function does inside |
| `model` | a points-to resolution that needs a pointer model |
| `closed-world` | rapid type analysis; the vtable rule on any other hierarchy; an interposable definition, a weak or exported alias, an ifunc (rule (c)); in component mode a function pointer that may come from outside its component |
| `unknown` | an unresolved or open call, an unclassified external |

For `never` the evidence is the weakest link over everything the function's closure rests on: every
call edge with its rule, every indirect call (also one without a target in the module), every external
the answer relies on, every interposable definition. This subsumes `none` and `none*` of the entry
report: `never` with evidence `own` or `direct` needs no resolution rule at all; up to `indirect-type`
it is the entry report's plain `none` (it rests on assumption 3 where an indirect rule is involved);
from `library-policy` on it is `none*`. For `may` the evidence is the weakest link of the strongest
witness; for `always`, of the strongest justification. Since every allocation site is a call of a
function the policy names, `may alloc` and `always alloc` are never better than `library-policy`; the
resolution rules a claim also uses are in `always.rests_on` and in the `callees` groups. Per `may` class
the table lists the callees that bring it, grouped by the evidence of the edge (`callees`); per `always`
class the own sites on every returning path and the callees whose `always` it uses (`always`).

**Attribution to the primitive: accounts.** An allocation is counted once, as `alloc`. What `malloc`
does inside (its arena lock, a possible `mmap`) is a *consequence* of that primitive: the site carries
`consequence_of: alloc` and is reported under the allocation, not as a lock or a system call of the
function. Rules, per site of class C at symbol s:

* `consequence`: s is a primitive of another class and not of C (`malloc` seen as a lock or syscall
  site, `pthread_mutex_lock` seen as a syscall site: the futex of the slow path, `operator delete` seen
  as an alloc site, `abort` or `__stack_chk_fail` seen as a lock site), or s is in another class's
  `attributed` list (throw: `__cxa_allocate_exception`, `__cxa_free_exception`, `__cxa_begin_catch`,
  `__cxa_end_catch`, `_Unwind_Resume`: a throw's allocation, release and unwinder lock);
* `library`: the site comes from the summary of a library function that is not a primitive (a
  `std::string` member, `std::locale`): something inside it may do C, and the summary does not say why.
  The summaries are too coarse to attribute this further (almost every libstdc++ function reaches every
  class through its termination paths);
* `own`: everything else: a primitive of C, another symbol the policy puts in C (`strdup`, `printf`'s
  stream lock, `__cxa_guard_acquire`), an instruction of C.

A function's account for C is the best account among all sites of C it can reach: `own` if it reaches
an own-account site, else `library`, else `consequence` (with the classes whose primitives bring it,
`consequence_of`). So a function is `lock: may` on its own account only if it reaches a lock primitive
or a lock symbol of the policy that is not a consequence of another class's primitive. The verdict
itself does not change: a function that only allocates is `lock: may`, account `consequence` (of
alloc), not `never`, because `malloc` does lock.

## Rules changed after the attack on the signatures (round 6, second part)

An adversary attacked the per-function signatures (every attempt is now a regression case
in `tests/sig/`). What held: the intra-function part of `always` (normal return, post-dominance, the
invoke rule, recursion), the pointer channels, coroutines, implicit C++ calls, component crossings, the
consistency of evidence labels. What broke came from seven causes, each fixed by a rule. Where an
earlier section of this README says otherwise, these rules replace it.

**(a) Closed-world claims: the vtable rule.** A virtual call is resolved by the vtable rule only if
every vtable defined in the module carries `!type` metadata. If one does not (bitcode not built with
`-flto -fwhole-program-vtables`, for example plain `-emit-llvm` bitcode collected by gllvm), its class
is invisible to the rule, so every vtable-rule call of the module is *open*: the targets found are kept,
the call is an `indirect-unknown` finding and the verdict is `unknown`. `module.vtables_without_type_metadata`
gives the number and the first names, and the text report says so. When all vtables have metadata, a
verdict that uses the vtable rule (at any precision, in module and in component mode, also as a
points-to fallback and as `points-to+ext:vtable`) has evidence `closed-world`, entry verdict `none*`,
unless the hierarchy is established as closed: the class has internal linkage (its type identifier is
not a name), or it has hidden LTO visibility (the call is guarded by `llvm.type.test` or a checked
load: `-fvisibility=hidden`, `[[clang::lto_visibility_hidden]]`; that is the build's own statement
that every derived class is in the LTO unit). Only then is the evidence `indirect-vtable`. A library
without bitcode that derives from a public class of the program is therefore never behind a plain
`none`. `always` through a virtual call uses the vtable rule's own target set, also when points-to
narrowed it with a pointer model, with evidence `indirect-vtable` or `closed-world` by the same test;
so component mode, which falls back to the vtable rule, prints the evidence module mode prints for the
same call or a weaker one (the suite compares the two tables of every case: `tests/sig/c00`).

**(b) A primitive is a library function.** A symbol pattern of a class, and a `primitives` pattern,
apply to a callee only if it has no body in the module (or the policy lists it as `opaque`: the user's
statement that a defined function is to be taken by its name). A function defined in the module is an
ordinary function whatever its name: the program's own `extern "C" send`, a user placement
`operator new(size_t, Slab&)`, a class-specific `operator new`, a replaced global `operator new`, an
allocator built from source. Its body decides, for `always`, for `may` and for the evidence. A replaced
global `operator new` is the program's allocator: a function that calls it is `alloc` exactly if the
replacement's body reaches an allocation primitive of the library (`malloc`, `mmap`, ...); a bump
allocator over a static arena is `never`. The three cases are thereby the same case. Library code that
calls the replaced operator (a `std::string` that grows) still counts by its summary, as `may`. The
run-time check counts as primitives only functions that live in a library.

**(c) Symbols that can be something else at link or load time.** One rule: a verdict that passes
through such a symbol has evidence `closed-world` (entry verdict `none*`), `always` included, and the
symbol is listed (`interposable_definitions`, `trusted`). The symbols: a weak, `linkonce` or common
definition; a function exported from a `-fPIC` shared object; an alias with weak linkage, or exported
from a shared object, whatever it aliases (the alias is the symbol the linker binds, the aliasee is
not); an ifunc (the call is resolved by type matching and marked closed-world: the resolver decides at
load time). `linkonce_odr` and `weak_odr` definitions are not on the list: every definition is the same
by the one-definition rule (assumption 1). An `available_externally` body is never used for `always`.

**(d) Calls that code generation adds.** The complete list for Clang 21 on x86-64, from module flags
and function attributes; each is a call of a library function, classified by its summary, and without a
summary it is `external-unknown` (the function's `never` becomes `unknown`):

| source | call | treatment |
|---|---|---|
| thread-local access, general- or local-dynamic model, TLS descriptors | `__tls_get_addr` | no summary: `unknown` (it allocates the block of a `dlopen`ed module under the loader's lock) |
| thread-local access with `-femulated-tls` (not recorded in the IR: the driver passes the flag on, `--emulated-tls` for bare bitcode; default on some targets) | `__emutls_get_address` | no summary: `unknown` |
| `ssp`, `sspstrong`, `sspreq` | `__stack_chk_fail` | summary |
| `-finstrument-functions`, `-pg` (`instrument-function-*`) | the named function (`mcount`, `__cyg_profile_func_*`) | summary |
| `-mfentry` (`fentry-call`) | `__fentry__` | a tracer's hook: `unknown` |
| XRay (`function-instrument`, `xray-instruction-threshold`) | patched sleds | `unknown` |
| `-fpatchable-function-entry` | whatever is patched in | `unknown` |
| `probe-stack` with a function name | that function | summary or `unknown` |
| `split-stack` | `__morestack` | policy class alloc; no summary: other classes `unknown` |
| `safestack` | SafeStack runtime | `unknown` |
| `fn_ret_thunk_extern`, target feature `retpoline-external-thunk` | external thunks | `unknown` |
| atomics wider than the lock-free width, misaligned; `frem`; wide division; math intrinsics | libatomic, libm, compiler-rt | as before ("Calls that appear only in code generation") |
| sanitizers, coverage, profile instrumentation | runtime calls | inserted by IR passes before the bitcode is written: the calls are in the IR |
| `shadowcallstack` | none (register operations) | no call |

A string function attribute that is neither in this list nor in `abi.known_fn_attrs` (attributes
reviewed as adding no call), and a module flag that is not in `abi.known_module_flags`, is reported:
`module.unknown_function_attributes`, `module.unknown_module_flags`, a line on stderr and in the text
report. It is not ignored and it is not silently trusted; it does not change a verdict by itself.

**(e) Inline assembly.** The text of a block is not interpreted. A block is harmless only if it is
exactly one of the strings in `abi.asm_harmless` (after whitespace is normalised): the empty block (the
optimisation barrier), `pause`, `rep; nop`, `nop`, `mfence`, `lfence`, `sfence`, `rdtsc`, `rdtscp`,
`cpuid`. Every other block is an unknown callee (`external-unknown`, subclass `inline-asm`): every
class of the function that has no other finding is `unknown`, `syscall` first of all, and nothing is
claimed `always` for the function. The class patterns (`syscall`, `lock`) only add a `may` where they
match; they never clear anything. `module.inline_asm` lists every block with its function and whether
it is harmless.

**(f) `always (elidable)`.** The theorem is about the analysed IR. The optimiser may afterwards remove
calls it knows to have no observable effect: `malloc`/`free` and `new`/`delete` pairs, an allocation
whose result is unused, `calloc`, `aligned_alloc`, `realloc(nullptr, n)`, `new[]`, `free(nullptr)` (all
eight patterns of `tests/sig/e01` disappear in an `-O2 -flto` link once the helpers of the other
translation unit are inlined). Whether a given call survives depends on what the link inlines, so the
rule is by family: a class may name `elidable` primitives (alloc: the `malloc` family and every
`operator new`; free: `free` and every `operator delete`), and an `always` claim every justification of
which uses an elidable primitive is `always (elidable)` (`qualifier: elidable` in the table). It holds
in the analysed IR and in an unoptimised build of it, and may be false in the shipped binary. An
`always` without the qualifier rests on primitives that are never removed (`posix_memalign`, `mmap`,
exception allocation, every lock, system-call and throw primitive) and must hold in both builds.

**(g) The run-time check** (`tests/sig/check.py`, `tests/sig/sig_gdb.py`, adopted from the adversary;
`tests/validate_always.py` drives them). Breakpoints are set with `-qualified` on library primitives
only, by address, so that `Class::operator new` and inlined copies of a name do not count. A normal
return is one where the frame's own `ret` instruction popped its return address: a `longjmp` or
`setcontext` landing is not one. Checked: every `always` claim (in the `-O2 -flto` build only those
without the qualifier), and every `never` whose evidence is that of a plain `none` (`own`, `direct`,
`indirect-pta`, `indirect-vtable`, `indirect-type`): no primitive of the class may be called during
any activation. For every test case this is run on the module-mode table and on the component-mode
table, each in two builds of the analysed bitcode, `-O0` and `-O2 -flto`. Negative control: three
injected false claims must be refuted.

## The rules, and why no behaviour of class C escapes them

The analysis reads the whole-program module that `llvm-link` makes from the pre-link LTO bitcode of
every translation unit. Every instruction of every function that is reached is examined (flow-insensitive
within a function: every block counts, reachable or not), so the arguments below are about edges between
functions and about calls the IR does not show.

**Direct calls and invokes.** A `call` or `invoke` whose callee is a function (through casts and aliases)
is an edge to it. With a body, the body is analysed. A call through a cast to another function type is
still the same edge. Tail calls (`tail`, `musttail`) are ordinary calls in IR. Nothing escapes: these
are all the call instructions there are, and the callee is a constant.

**Virtual calls.** Compile with `-flto -fwhole-program-vtables`. Clang then puts `!type` metadata on every
vtable at every address point and guards every virtual call with `llvm.public.type.test` (classes with
public LTO visibility) or `llvm.type.test` (with `-fvisibility=hidden`, `-flto-visibility-public-std` or
`[[clang::lto_visibility_public]]`), or `llvm.type.checked.load` (with `-fvirtual-function-elimination`
or CFI). LLVM's `findDevirtualizableCallsForTypeTest` links each type test to the call sites that use the
tested vtable pointer and the slot offset. The targets are the functions at that offset in every vtable
of the module that carries the same type id: by the C++ object model (assumption 1) the dynamic type of
the object derives from the static type, its vtable has the static type's id at the address point used,
and the slot holds the final overrider. This is complete when every class derived from the static type
is in the module. Where it cannot be: the type is in namespace `std`, `__gnu_cxx` or `__cxxabiv1`
(policy `abi.open_types`), or the class's own vtable is only declared in the module (it lives in a
library), the call is `open`: the module's targets are kept and the call is also an `indirect-unknown`
finding, so the verdict is `unknown`. Example: `shared_ptr`'s `_Sp_counted_base::_M_dispose`, whose
derived classes include some defined inside `libstdc++.so`. No vtable with the type id at all:
unresolved, `indirect-unknown`. Which visibility flags are needed: none for this analysis. The two kinds
of type test carry the same information. What the flags change is the final LTO link (it only
devirtualises public types with `--lto-whole-program-visibility`), and so the code that runs, not the
argument. Without `-fwhole-program-vtables` there is no type metadata; virtual calls then fall to the
function-pointer rule below (sound, much less precise: every address-taken function of that IR type,
and every vtable entry is address-taken).

**Calls through a pointer loaded from a constant table.** If the callee is loaded from a global that is
`constant` with a definitive initializer (and not thread-local), the targets are the functions in that
initializer, restricted to the call's IR function type when that leaves any. The loaded value can only
be one of the initializer's values (assumption 1: no out-of-bounds read).

**Calls through function pointers (including `std::function`).** Targets are all address-taken functions
(bodies and declarations) whose IR function type equals the call's. Address-taken means any use other
than being the callee of a call: a store, a constant initializer (vtables included), an argument, a
cast. A function pointer that points into the module can only come from such a use, and calling it with
another type is undefined behaviour (assumption 1). With opaque pointers the IR type is the ABI-lowered
signature, which is the same in the caller and the callee. `std::function`: the invoker is called
through `R(const _Any_data&, Args&&...)`, the manager through `bool(_Any_data&, const _Any_data&, op)`.
Every lambda or functor ever stored in a `std::function` of that signature is a target. How sound:
complete for pointers created in the module under assumption 1. Not complete for pointers created in a
library without bitcode (a pointer returned by a library function, or written by a library into memory
the module reads). This is the `whole program` part of assumption 3, printed in `global_assumptions`.
If the module calls `dlsym` or `dlvsym`, every call resolved by this rule is marked open (`unknown`).
How precise: poor on real programs. `void(ptr)`, `void(ptr, ptr)` and the `std::function` manager types
gather hundreds of functions. On the largest project measured (below), dropping these edges cut what a
hot entry reaches from 2081 functions to 510. `resolution.fnptr_match` can widen the rule to same
arity, or to any address-taken function, for code that calls through mismatched pointer types (C
callbacks with casts). It cannot narrow it.

**Callbacks from external functions.** A call to a function without a body also gets an edge to every
function passed to it directly as an argument (`qsort` comparators, `__cxa_throw`'s destructor argument,
`pthread_once` routines), except for the registration-only functions in `abi.registration_only`, whose
callback runs later or on another thread: `atexit`, `pthread_create`, `signal`, `sigaction`, ... A
library that stores a callback and calls it during a later call is not covered (printed assumption);
summaries should report such calls as unresolved indirect calls.

**External functions and summaries.** A function with no body is classified in this order: (1) the
classes whose symbol patterns match it; (2) for a class defined as "every external except this list"
(`external_except`, used by the signal-safety policy), membership; (3) the policy's `known` list (no
class); (4) the policy's `known_if_call_attrs` (the noexcept policy accepts `nounwind` declarations);
(5) a library summary (`--summary`, JSON, one entry per exported symbol): its classes map onto the policy's classes
by name (`summary_classes`), and `unresolved_indirect > 0` makes it `unknown` too; (6) a function defined
in a translation unit that did not compile (`tu-not-compiled`, from the original object's symbol table);
(7) otherwise `external-unknown`. The pattern tables are the soundness-critical part. A symbol listed as
`known` that does allocate makes a `none` wrong, which is why every `known` symbol a verdict depends on
is listed in `relies_on_externals`. Pattern matching applies only to functions without a body in
the module (rule (b) above): a user `operator new` or an allocator built from source is analysed by its body. `opaque` (policy) declares
functions to be leaves even if they have a body: an arena's `allocate` that may refill from `malloc`
is then not followed. That is the user's statement, and it is visible in the policy file.

**Inlined code.** Inlining copies the callee's instructions into the caller, so they are analysed there.
Their debug locations keep the `inlinedAt` chain: a finding inside an inlined callee names the callee's
line (`site.file`/`line`, `scope`) and every caller line (`site.inlined_at`).

**Invoke and unwinding.** Landing pads, cleanups and catch handlers are ordinary blocks of the function,
so destructors run during unwinding and code in `catch` blocks are analysed like any other code. The
unwinder and the C++ runtime work behind a throw: `_Unwind_RaiseException` looks up unwind tables
(`dl_iterate_phdr`, which takes the loader lock with older glibc and libgcc). `__cxa_allocate_exception`
falls back to an emergency pool behind a mutex. The exception globals are thread-local in
`libstdc++.so`. The policy counts these as `lock` and `tls-dynamic` sites of every throw (marked
"possible"). Personality routines run inside the unwinder and are covered by the same entries.

**Intrinsics.** An intrinsic may become a library call in code generation: `llvm.memcpy/memmove/memset`
become `memcpy/memmove/memset`. The math intrinsics (`llvm.sin`, `llvm.pow`, and also `llvm.floor`,
`llvm.ceil`, `llvm.rint`, `llvm.fma` and others, which have no instruction on baseline x86-64 without
SSE4.1 or FMA) become libm calls, and `llvm.powi` becomes compiler-rt. `abi.intrinsic_libcalls` maps
each such intrinsic to its possible calls, which are then classified like any external.
`abi.intrinsics_no_call` lists the intrinsics that never call (lifetime markers, `llvm.assume`, bit
manipulation, overflow arithmetic, target intrinsics, ...; reviewed entry by entry in round 5, rule R4).
An intrinsic in neither list and in no class is `external-unknown`.

**Calls that appear only in code generation.** These are the calls the IR does not show: `frem` becomes
`fmod`/`fmodf`/`fmodl`; atomic operations wider than the target's lock-free width (8 bytes on baseline
x86-64, 16 with `+cx16`), of non-power-of-two size, or aligned below their size (a member of a packed
struct) become `__atomic_*` calls into libatomic, which uses locks. A function with `ssp`/`sspstrong`/`sspreq` calls `__stack_chk_fail` (cold);
`instrument-function-entry`/`-exit` attributes (`-finstrument-functions`, `-pg`) call the named
function, and `split-stack` calls `__morestack`. These come from `abi.attr_libcalls` and are classified
like external calls. Integer division wider than 64 bits and `fp128` arithmetic call compiler-rt
(`__divti3`, `__addtf3`, ...: `known`, so a `none` past them is `none*`). Not modelled: the conversions
between `i128` and floating point (`__fixdfti`, ...) and stack probes.

**Global constructors and destructors.** They run before `main` or after `exit`, so they are outside an
entry's call tree (assumption 5), with one exception, handled: an entry that reaches `exit` or
`quick_exit` gets edges to every function registered with `atexit`/`__cxa_atexit`/`at_quick_exit` in
the module and to every function in `llvm.global_dtors`. A constructor that installs a function pointer
(a hook) makes that function address-taken, and the function-pointer rule picks it up.

**Function-local statics and thread-local initialisers.** The guard check, `__cxa_guard_acquire`, the
initialiser and `__cxa_atexit` are ordinary IR in the function (tagged `first-use`, section "Tags"). A `thread_local` with a dynamic initialiser is accessed through its wrapper (`_ZTW...`),
which calls the initialiser (`_ZTH...`, the constructor and `__cxa_thread_atexit`). An `extern
thread_local` whose initialiser is in a translation unit without bitcode is an `extern_weak` declaration
of `_ZTH...`, hence `external-unknown`.

**Thread-local access model.** Each access through `llvm.threadlocal.address` (or a direct operand) gets
the model the code generator will choose, mirroring `TargetMachine::getTLSModel`: the module's PIC/PIE
level, `dso_local`, and an explicit model in the IR. General- and local-dynamic accesses are
`tls-dynamic`. A PIE or static executable gets local- or initial-exec. A `-fPIC` object without PIE
gets general-dynamic. Linker relaxation can only make it cheaper.

**Inline assembly.** Rule (e) above: a block is harmless only if it is exactly on the list
`abi.asm_harmless`; every other block is `external-unknown`. The policy's `asm` patterns (`syscall`/`int
$0x80`: `syscall`; `lock` prefix: `atomic`) add findings where they match and clear nothing. Hand-written
assembly files are outside the module (assumption 4).

**Instruction-level classes.** `atomicrmw`, `cmpxchg`, `fence`, atomic loads and stores (`atomic`,
informational); `landingpad` and `resume` (`unwind`, informational); `alloca` with a non-constant size
(`alloca-dynamic`); `unreachable` not preceded by a trap or a noreturn call (`ub-unreachable`,
informational). `llvm.trap`, `llvm.ubsantrap` and `llvm.debugtrap` (libc++ hardening in fast mode,
`__builtin_trap`, `__builtin_debugtrap`: `int3` kills the process with SIGTRAP when no debugger is
attached) are `abort/trap`.

**Recursion.** Strongly connected components (Tarjan) of the call graph of defined functions, with every
edge kind. A call edge inside a component of more than one function, or to itself, is a `recursion`
site. Over-approximated edges can create spurious cycles. They never hide one.

**Effect table and entry search.** `effects.json` holds, for every defined function and class, one
witness (a local site, or the callee edge it came from), computed by propagating local sites backwards
over the call graph to a fixed point. A second and a third propagation over hot edges only give the
hot and hot-or-unknown tags. The per-entry report is a breadth-first search from the entry that lists
every site. The plugin checks for every entry that both give the same set of reachable classes and of
hot classes (`table_agrees`). A disagreement is reported as an internal error.

## Indirect calls: three precision levels

`resolution.precision` in the policy, or `--precision type|rta|pta` (default `pta`). Every indirect call
site in the report (`indirect_calls`) carries the rule that produced its targets; chain edges are labelled
`virtual:<rule>` / `fnptr:<rule>`.

* `type`: the rules above (vtable slots of every vtable with the type id; constant tables; every
  address-taken function of the IR type).
* `rta` (rapid type analysis): as `type`, but a vtable that nothing in the module refers to (no
  constructor stores it, no VTT or static object contains it) is dropped from the virtual-call targets.
  Why it is only `none*` (closed world): an object of class D gets D's vtable pointer only from D's
  constructor, but a library compiled with D's header can construct one, and a translation unit that did
  not compile can too. A call whose targets RTA pruned is listed as `closed-world` in `trusted`; with a
  failed translation unit RTA is off (rule R5).
* `pta`: `rta`, then the points-to analysis below decides each indirect call it can.

### Points-to analysis (`src/PointsTo.cpp`)

Algorithm: inclusion-based (Andersen), context-insensitive, field-sensitive by byte offset, over the
whole linked module, every function (reachable or not), with the call graph built on the fly.

* Abstract objects: each function, each global (its initializer is its initial contents, so vtables and
  callback tables are just memory), each `alloca`, each heap allocation site (calls to the `fresh`
  functions of `abi.pointer_models`: malloc, operator new, ...), and two special objects: EXT (memory and
  code of libraries without bitcode, and everything that has escaped to them) and UNK (a pointer of
  unknown provenance). A points-to element is (object, offset) or (object, any offset).
* Values: every SSA value of every type has a points-to set, integers and floats included: a pointer
  converted to an integer keeps its provenance (`ptrtoint`, arithmetic, `inttoptr`, stores of `i64`,
  vector copies). Constant offsets (`getelementptr`) shift the offset; variable offsets and arithmetic
  make it "any"; more than 64 offsets in one object collapse it to field-insensitive.
* Memory: a store of a value of size s at (o, k) writes slot (o, k, s); a load of size s at (o, k) reads
  every slot of o that overlaps [k, k+s), and the "any offset" slot; a partial or misaligned overlap keeps
  provenance but loses the offset. `memcpy`/`memmove` (intrinsic or libc) copy the slots in range.
* Calls: direct calls bind arguments to parameters and the return value; an indirect call does the same
  for every function that appears in the callee's set (that is how `std::function` resolves: the
  constructor stores the manager and the invoker into the object, and `operator()` loads the invoker from
  the same object; a lambda converted to a function pointer is its static invoker's address; a virtual call
  loads the vtable pointer that the constructor stored and then the slot from the vtable's initializer).
* Library functions (rule R2): `abi.pointer_models` says what a function without a body does with
  pointers (`none`, `fresh`, `realloc`, `copy`, `ret_arg0`/`ret_arg1`, `data` for container internals
  within a given size, `ret_ext`, `throw`/`catch` for the in-flight exception, plus the effects
  `writes_bytes`, `exposes`, `sends`, `permutes` and `calls`). Anything not listed *escapes*: its
  pointer arguments, and everything reachable from them, become reachable (and writable) by library
  code, and its result is EXT.
* Escape: an escaped object may contain EXT (library code can write anything it has into it); an escaped
  function may be called by library code with EXT arguments; `main`, functions in `abi.called_from_outside`,
  and every exported function of a `-fPIC` shared object are escaped from the start, as are declared,
  weak and (in a shared object) exported globals. EXT stands for "any escaped object or library object":
  a load through EXT gives EXT, a store through EXT makes the value escape. A value narrower than a pointer
  (`i32`, `float`) cannot hand a pointer to library code and does not escape across the boundary
  (inside the module it keeps its provenance).

* Integers (rule R1): provenance flows through copies, and through `add`, `or`, `and`, `xor` and the
  first operand of `sub` with the offset lost; every arithmetic result also carries NOPROV. A store of a
  type without provenance (`i32`, floating point) writes NOPROV into its bytes.
* `inttoptr`: UNK, plus the integer's provenance. A constant address in the first page (a null-pointer
  access left by the optimiser on a dead path) points to nothing. UNK and NOPROV can point into any
  *exposed* object (one whose address went through `ptrtoint` or escaped): a load through them gives UNK,
  a store through them writes into every exposed object (one hub node) and escapes.
* Context sensitivity where it matters most: a direct call to a small function (40 instructions or fewer,
  not recursive, nesting depth 2) gets its own copy of the callee's constraints, locals included; the
  shared copy keeps indirect and library callers. Exact, hence sound.
* `std::string`: its members, `operator+`, `__str_concat` and `to_string` over `char` strings are modelled
  in the points-to analysis even where their bodies are in the module (`"bodies": true`): they write into
  the string they are given only pointers into that string and fresh heap pointers (`self`). Without this
  the small-buffer self-pointer, stored by functions shared by every string of the program, makes every
  string point to every other one. Member templates over user iterators are excluded (they run user code).
* Budget: `-fx-pta-budget=<seconds>` abandons the analysis when exceeded; every indirect call then keeps
  the `rta` rule, and the report says so (`module.points_to.over_budget`).

Per indirect call, after the fixed point:

1. the callee set contains UNK or NOPROV (an `inttoptr`, a function address plus an offset, a slot
   written with an integer without provenance, bytes from a `read`, an IFUNC, a relative-vtable load):
   fall back to the `rta` rule for this call (`<rule>+fallback(unknown provenance)`);
2. no function and no EXT reaches the callee: fall back (`+fallback(no pointer reaches)`); the arguments
   are then bound to the fallback targets and the analysis is solved again, so that it does not depend on
   a call it reports as unresolved;
3. otherwise the targets are the functions in the set whose IR function type equals the call's
   (calling through another type is undefined behaviour, as for the type rule), intersected with the
   vtable or constant-table rule when that rule resolved the call (if the intersection is empty while
   the set is not, fall back: `+fallback(points-to disagrees)`; it never happened in the tests);
4. if EXT is in the set (`points-to+ext`): add every escaped function of the call's type; the call may also
   reach library code, so it is open (`indirect-unknown`, verdict `unknown`) unless
   `resolution.ext_callee` is `assume-module` (the round-1 assumption that library code hands out no
   function pointers of its own).

Soundness argument, in one line per construct: every way a pointer value can move in LLVM IR (SSA,
memory, calls, returns, exceptions (`landingpad` gets every thrown object and EXT), varargs (`va_arg` gets
every variadic argument and EXT), atomics, intrinsics) has a constraint above. Every way it can enter or
leave the module (library calls, functions and data visible to libraries, inline assembly (treated as an
unknown library call), integers of unknown origin) goes through EXT or UNK. A call whose callee set
contains EXT or UNK, or nothing at all, never gets a smaller target set than the round-1 rules, except
by the EXT rule of step 4, which relies on escape being complete. The pointer models are trusted data
(like summaries): a verdict whose indirect calls depend on them is `none*` (rule R3).

What the analysis assumes, beyond the five assumptions: library code honours the documented pointer
behaviour of the functions listed in `abi.pointer_models` (and a `none` that depends on it says so); a
compile-time integer constant below 65536 is not (part of) an object's address unless it is stored with
a type that carries no provenance; the program's code is the module (assumption 2), so no other module
writes into its non-exported globals.

Known imprecision: context-insensitive (one points-to set per function parameter: a constructor or
accessor that is not inlined merges every object it is called with; test `pta_precision` uses inlined
ones); flow-insensitive (a slot holds everything ever stored into it, so a `std::function` that is
reassigned has every value it ever held); arrays are one object (the index is "any offset").

### Points-to per component (`--pta-scope component`, round 6)

The whole-module solve does not reach its fixed point on the larger field programs. In component mode
the same solver treats the module as the disjoint union of its components (the strongly connected
components of the direct-call graph), which is the bottom-up structure of the signatures:

* a call from one component into another is not bound to the callee. It is treated exactly as a call
  of an unknown library function with the default model: the arguments escape, the result is EXT;
* every function may be called from outside its component: its parameters hold EXT, its result escapes;
* every mutable global is visible to the other components: it escapes (constant globals, vtables among
  them, keep their initializers);
* an exposed object (one whose address became an integer) escapes, so the hub that carries stores
  through pointers of unknown provenance is not needed: what such a store writes escapes, and EXT in an
  escaped object stands for it;
* a small callee still gets its own copy of its constraints at a direct call (the existing rule): that
  copy is the callee's exact signature. For every other callee the signature is the weakest one,
  "unknown".

Per indirect call: if the callee set contains EXT (which here also stands for every other component)
or UNK, or is empty, the call keeps its own rule (`<rule>+fallback(outside the component)`); otherwise
the functions in the set are the targets (`points-to`), as in module mode. A function-pointer call
that falls back this way is a closed-world claim (evidence `closed-world`, entry verdict `none*`): the
pointer may come from a library, and the type rule covers only pointers made in the module. A virtual
call that falls back follows rule (a).

Why it is sound for any partition: each of the first four points replaces constraints by ones the
existing analysis already uses for code it cannot see, and the argument for those (the points-to
section: everything that crosses the boundary goes through EXT, escape is closed) does not depend on
whether the unseen code is a library or another component. A value that reaches a call from another
component can only come through a parameter, a call result, a mutable global, an escaped object or the
in-flight exception, and each of those holds EXT. The partition decides only the precision. Unlike
module mode, a callee that may be EXT is not reported open (`unknown`): it falls back to the rule the
`rta` level would have used, and labels the result closed-world. What is lost against a finished
module solve is the report that a callee may be library code (test `pta_adversarial`, `via_library`:
`unknown` in module mode, `never` with evidence `closed-world`, entry `none*`, per component). It is
the one place where the component table says more than the module table, and it says it as a
non-guarantee (`tests/sig/c00` pins that there is no other). `--pta-scope auto` runs the module solve first and the component solve
when the first is over its budget. Every test case is also run in component mode: no class may be
`none` there where the module solve finds it `reachable`, and the RealtimeSanitizer comparison is
repeated on that report.

What it does not do: there are no symbolic callee signatures (a callee's effect on memory it reaches
through its parameters, to be instantiated at each caller). A call through a member of `*this`, through
a global hook, or through anything a non-inlined callee was given cannot be resolved this way.

## Tags

A tag says what the compiler's hints say about a site's path. It never changes a verdict (rule R6). The
rule, per function:

1. Cold seed blocks: a landing pad (any EH pad); a block that ends in `unreachable` or `resume`; a block
   that contains a call with the `cold` attribute (on the call or the callee), a call to a function
   matching `abi.cold_targets` (`__cxa_pure_virtual`), or a `noreturn` call whose callee is external or
   indirect, or is defined and has a cold entry block. A defined `noreturn` function whose entry is not
   cold (an event loop) does not make its callers cold.
2. Backward closure: a block all of whose successors are cold is cold (to a fixed point).
3. Unlikely edges: an edge of a branch or switch whose `branch_weights` give it a probability below
   `temp.cold_branch_probability` (0.002). `__builtin_expect` and `[[unlikely]]` give 1/2001; the
   static-guard check gives 1/1048576. A branch on an unlowered `llvm.expect` (at -O0) counts too.
4. The likely blocks are those reachable from the entry block without entering a cold block or taking an
   unlikely edge.
5. First-use blocks: those reached from a likely block through the not-yet-initialised edge of a guard
   (a branch on a `_ZGV...` guard variable of a function-local static or thread-local, or on
   `__tls_guard`), and from there through edges that are not unlikely, and not likely themselves.
6. Every other block is unlikely. A function with the `cold` attribute is unlikely throughout.

A site's tag: `likely` if its block is likely and a path from the entry reaches its function through
likely call sites only; `first-use` if it is in a first-use block, or its function is reached only
through first-use call sites (and its own block is not unlikely); otherwise `unlikely`. A class that
comes from a library summary at a likely call site is `unknown` (the library's internal paths are not
visible). The targets in `abi.cold_targets` are `unlikely` wherever they are called.

What `unlikely` means: the compiler's branch hints say so, nothing more. A `__builtin_expect(x, 0)`
branch that the program takes on every fourth call is `unlikely` (attack 14); so is every path behind a
`[[unlikely]]` the programmer got wrong, and every path a profile marked rare. `first-use` sites run once
per process or per thread, and that once can be inside the hot path (the first request of each worker
thread). Neither is a statement about how often a path runs. Counts per class: `likely`, `unlikely`,
`first_use`, `unknown`. Tests: `error_path`, `static_local_guard`, `thread_local_ctor`,
`vector_at_assert`, `attack_14_cold_wrong`.

## Library summaries

The driver loads `summaries/*.json.gz` when that directory exists (`--summaries-dir`,
`--no-default-summaries`): libc, libm, and the libstdc++ summary whose `path` is the file Clang links for
this build (with the project's GCC installation, that is the system one). A summary's classes are mapped
onto the policy's classes through each class's `summary_classes` (default: the same name; the hot-path
policy maps `thread` onto `syscall`), and are added even to symbols the policy already classifies:
`malloc` also locks and may call `mmap`. A class whose summary verdict is `unknown` becomes `unknown` for
that class only (the finding is of class `unknown:<class>`), not for every class. A policy can refuse
summaries for a class (`"summary_classes": []`): the signal-safety policy does that for `tls-dynamic`,
because for library functions the POSIX list is the authority.

## vDSO calls

The hot-path policy puts `clock_gettime`, `gettimeofday`, `time`, `clock_getres`, `getcpu` and
`sched_getcpu` in the informational class `vdso`: on the common path they read a page the kernel maps into
the process and do not enter the kernel. They do make a real system call:

* `clock_gettime`/`clock_getres` with `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`, a CPU clock
  from `clock_getcpuclockid`/`pthread_getcpuclockid`, `CLOCK_REALTIME_ALARM`, `CLOCK_BOOTTIME_ALARM`, or an
  invalid id (the vDSO serves `CLOCK_REALTIME`, `CLOCK_MONOTONIC`, `CLOCK_BOOTTIME`, `CLOCK_TAI`,
  `CLOCK_MONOTONIC_RAW` and the `_COARSE` clocks);
* every high-resolution clock, `gettimeofday` included, when the current clock source has no vDSO mode
  (for example `hpet` or `acpi_pm`, which a kernel switches to when the TSC is marked unstable; some
  virtual machines), or when the vDSO is disabled (`vdso=0`).

Round 5 (rule R3): the binary analyser does not count the fallback; its summary names it per function
(`vdso.fallback_syscalls`). fn-effects adds a `syscall` site (`vdso fallback: clock_gettime`, tag `unknown`)
for every such call, so `syscall` is `reachable`: the fallback is taken for CPU-time clocks and whenever
the clock source has no vDSO mode, which the program cannot rule out. A vDSO function whose summary does
not name the fallback, or that has no summary, makes `syscall` `unknown`. `clock_getcpuclockid` is in
the `syscall` class (glibc validates the clock with `clock_getres`).

## Report

The JSON report is compact by default: per entry the verdicts, counts, violations, one finding per
(class, subclass, symbol) with its hottest site and one chain, the ids of the assumptions it rests on
(into the top-level `assumptions` table) and of the indirect calls (into `indirect_calls`). `--full` lists
findings per function with every site, and per-function lint. The text report is one screen per entry
(`--text-full` for the long form).

## Policy file

JSON. `include` (paths relative to the file) merges other policies: classes by name replace, or extend
with `"extend": true`; lists are appended. Keys: `classes` (`symbols`, `intrinsics`, `exclude`,
`external_except`, `instructions`, `asm`, `summary_classes`, `informational`, `primitives`: library functions
every call of which is an operation of the class, `attributed`: symbols whose sites in other classes are
consequences of this class), `known`,
`known_if_call_attrs`, `prune_call_attrs` (call sites with this attribute are not followed: the noexcept
policy stops at `nounwind` calls), `opaque`, `abi`, `lint`, `temp`, `resolution`, `entry_kinds`
(`select`, `forbid`, `start`: `all` or `terminate-invokes`), `assumptions`. Patterns: exact, glob, or
`re:` (POSIX ERE), each optionally `dem:` to match the demangled name. A pattern can carry `subclass`.
Write regexes with an anchored literal start (`^name` or `^(a|b|c)`): the engine then filters names by
prefix before running LLVM's regex engine, which is slow on long alternations.

Instruction kinds: `atomicrmw`, `cmpxchg`, `fence`, `load-atomic`, `store-atomic`, `landingpad`, `resume`,
`unreachable-bare`, `alloca-dynamic`, `tls-dynamic`, `tls-static`.

## Reproducibility lint

For each reached function: counts of fast-math flags on floating-point instructions (`reassoc`, `nnan`,
`ninf`, `nsz`, `arcp`, `contract`, `afn`), `llvm.fmuladd` calls (contraction, `-ffp-contract=on` is
Clang's default), `llvm.fma`, calls into libm (split into correctly rounded ones, such as `sqrt` and
`floor`, and the others, such as `sin`, `exp` and `pow`, whose results differ between library versions),
and the floating-point function attributes (`unsafe-fp-math`, `denormal-fp-math`, ...), with the
function's translation unit.

## Existing projects (`-p compile_commands.json`)

Each command is re-run with Clang: the compiler, output and dependency flags are replaced, and
`-flto -fwhole-program-vtables -gline-tables-only` are added. A known list of GCC-only flags is dropped
(`-fno-tree-*`, `-fipa-*`, `-flto=auto`, `-fprofile-use`, ...), and a flag Clang still rejects is dropped
on a retry. Every dropped flag is reported. When the project was built with a GCC installation, Clang is
pointed at it (`--gcc-toolchain=<prefix>`, plus `--sysroot=/` for `/usr`), so it sees the same libstdc++
and the same system headers. A file listed twice (one source in two targets) is analysed once. A file
that still does not compile is skipped. The functions it defines (from the original object file, if
the build left one that `llvm-nm` can read) become `external-unknown` with subclass
`tu-not-compiled:<file>`. Several programs in one database collide at link time; select one with
`--cc-filter`/`--cc-exclude`.

## Tests

Round 5: every directory of the adversary's attempts is a regression case `tests/cases/attack_NN_*`
(sources copied; each hazard the adversary's run showed must be `reachable`, `unknown` or `none*`;
`attack_19` is the exception explained in its case file, with `attack_19b` testing the intrinsic at the IR
level), plus `provenance_rules` (union punning, 32-bit halves, function-address arithmetic, and the
precision the strict-aliasing refinement keeps) and a `--strict` variant of `fnptr_table`.

`python3 tests/run_tests.py` runs every case in `tests/cases/*/case.json` (sources, entries, expected
verdicts and findings, written before the first run; where the first run showed the expectation was
wrong, the case says so in a note), the `--diff` checks, the baseline workflow, and the
RealtimeSanitizer comparison: each case is built again with `-fsanitize=realtime` and entries marked
`[[clang::nonblocking]]` (`-DHP_RTSAN`), run with `halt_on_error=false`. Every event must be covered by a
finding of its class, or by an `unknown` verdict, in the static report of that same build. Results go
to `tests/out/` (`results.txt`, `rtsan-comparison.txt`).

Round 6 adds: the case `always_rules` (every `always` rule and its `may` neighbour: every path, one
branch, a loop that may run zero times, an early return, `push_back`, `reserve(n)` with n possibly zero,
the Liquibook shape, a callee that is always called, virtual calls with one or all targets allocating,
calls through pointers under points-to and under type matching, a weak callee, a throw path, a caught
throw, an invoke whose handler returns, `setjmp`, loops that may not terminate, a noreturn branch,
`musttail`, a cycle, a system call, an atomic instruction; expectations on verdict, evidence and
account); for every entry of every case a check that the entry verdict agrees with the function's
signature; the run-time check of every `always` claim with its negative control
(`tests/out/always-validation.txt`); and a second run of every case with points-to per component (no
`none` where the module solve finds the class reachable; the RealtimeSanitizer comparison repeated).

Round 6, second part: `tests/sig/<attempt>/` holds the adversary's 23 attempts (sources, `run.sh`,
`expect.txt`), run by the suite after the cases: no line of their run-time evidence may be a
counterexample to a claim, and `expect.txt` pins the verdicts that changed (a line starting with `!`
must not match). `tests/sig/check.py` is the run-time checker of rule (g). A case can opt out of the
run-time check with `runtime_check` in its `case.json` and a reason (`guarantee_ub_cast`: undefined
behaviour on purpose).

## Known limits

* Function pointers created inside libraries without bitcode are not targets of the function-pointer
  rule (assumption 3, printed). With `dlsym` in the module, such calls become `unknown`.
* Calls through a function pointer of the wrong type (undefined behaviour) are missed with
  `fnptr_match: exact` (test `guarantee_ub_cast`).
* `catch` clauses are not matched against thrown types: the noexcept policy reports a throw that a
  handler on the way would catch.
* The analysis is of pre-link bitcode. The final LTO link can remove calls, devirtualise and inline.
  It can turn `malloc`+`memset` into `calloc` or `printf` into `puts`, which are other members of the
  same classes. It cannot add a call of a new class, except the code-generation calls listed above.
* What the binary analyser checks and this one cannot: the shipped binary, PLT/lazy binding,
  `.cold` placement, assembly files.

## Diagnostics (environment variables, for development)

Round 5 adds: `FX_TRACE_NOPROV=1` (with `FX_PTA_DEBUG=<function>`: where the bytes without provenance in
a callee's set came from, and the first pointer loads that received them), `FX_TRACE_UNKSTORE=1` (the
first stores through a pointer of unknown provenance, with their instruction), `FX_DESC_NODES=<ids>`.

`FX_PROFILE` (phase and per-function timings, points-to progress and largest sets), `FX_PTA_DEBUG=<fn>`
(points-to set of each indirect callee in matching functions), `FX_PTA_TRACE` (order in which objects
escape), `FX_TRACE_OBJ=<fn>` (path by which the locals of matching functions reach the largest set),
`FX_PTA_MAX_OFFSETS` (offsets per object before it collapses, default 64), `FX_CHECK_PREFILTER` (check the
regex prefilter against the full regex), `FX_DIAG_DROP=fnptr,virtual,callback` (drop those edges:
unsound, for measuring which rule causes what).
