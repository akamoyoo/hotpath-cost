# Taut

A systems language for code that has to be both fast and dependable, built on one idea:

> **A function's signature tells the whole truth, and every guarantee has a floor.**

**Status: design stage.** There is no compiler yet. This repository holds the design draft, and it will change.
The current draft is [DESIGN.md](DESIGN.md) (version 0.2, written in Chinese). A review of that draft found soundness gaps and
under-specified rules; version 0.3, in English, will address them.

## The problem

In C++ a signature tells you types. In Rust it also tells you ownership. Neither tells you whether a function
allocates, blocks, takes a lock, can abort, loops without bound, or indexes out of range. On a hot path those are
exactly the properties that matter, and today they are kept by convention, code review and reading assembly.
They also break silently: a library or compiler upgrade can add an allocation without changing any signature.

## The idea

Taut puts those properties in the signature and has the compiler check them.

- **Sizes and ranges.** Array types carry symbolic sizes; integers can carry ranges. Indexing, integer overflow,
  narrowing conversions and integer division create proof obligations that are discharged automatically in a
  decidable fragment (linear integer arithmetic). If an obligation cannot be proved, compilation fails with a
  counterexample. The only way to turn a run-time fact into a compile-time one is an explicit branch, so there are
  no hidden checks.
- **Ownership.** Value semantics with exclusive mutable access: read-only, exclusive, or moved. No lifetime
  annotations, no global mutable state.
- **Effects.** Allocation, locking, blocking, calls into the host, aborting, unbounded loops and permission to
  reorder floating-point operations are effects. A function's effects must be covered by its signature.

Each function is checked once, against its own signature and the signatures of what it calls. Properties therefore
compose: they hold for the whole call tree, and an implementation can be replaced without rechecking its callers.

"Has a floor" means two things. Inside the language there is no undefined behaviour and no unchecked escape hatch.
And everything that is assumed rather than proved is listed in a machine-readable **trust manifest** emitted with
every build.

## Components and hosts

The unit of delivery is a **component**: self-contained code that exports functions and declares the interfaces it
needs from its host. It can reach nothing else. There is no ambient access to files, the network, the clock, the
allocator or threads; each of those arrives as an imported interface passed in by the host.

A host can be a C or C++ program, an operating-system platform (the whole program is written in Taut), or a test,
simulation or replay harness. A component is shipped as a library plus a generated header using the C calling
convention, so it can be called from C, C++ and anything else that speaks that convention.

```
component book level(realtime)

// What the host must provide. The contract is a requirement on the host's implementation.
import interface TradeSink {
    fn on_trade(px: i64, qty: u32) nonblocking noalloc bounded
}

struct Level { px: i64, qty: u64 }
enum ApplyError { Truncated, UnknownType, BookFull }

// Stateful object: capacity is fixed at creation, nothing is allocated afterwards.
export struct Book[cap]

export fn init[cap](storage: uninit Book[cap])
export fn apply[cap, n](book: inout Book[cap], msg: [n]u8, sink: TradeSink)
        -> Result[(), ApplyError]
export fn best_bid[cap](book: Book[cap]) -> Level?
```

The syntax above is a sketch, not a decision.

Only plain data crosses the boundary: scalars, fixed-layout structs, array views (pointer and length) and opaque
handles. Taut code cannot call arbitrary C or C++ directly, and it does not share C++ objects, templates or class
hierarchies with its host. An existing library is used by having the host wrap it behind an imported interface.

## What is meant to be guaranteed

The design goal is a soundness theorem of this shape.

**Assuming** that callers respect each exported function's entry contract (the checkable part can be verified at
the boundary), that the host's implementations of imported interfaces meet their declared contracts, that code
outside the component does not corrupt the component's memory, and that the trusted declarations and the toolchain
are correct —

**then** in every execution of a component:

1. there are no out-of-bounds accesses, uninitialised reads, dangling references or data races;
2. there is no undeclared integer overflow, lossy conversion or integer division by zero;
3. no function does more than its signature's effects allow;
4. every call at the two strictest levels returns within a statically bounded number of steps;
5. linear resources are consumed exactly once;
6. functions without the nondeterminism and reassociation effects return bit-identical results for identical
   inputs and identical results from imported calls.

For a program written entirely in Taut, the assumptions shrink to one: the standard library's bindings to the
operating system are correct.

This is a goal, not a result. Formalising the core calculus and proving the theorem is part of the work.

## Strictness levels

A level is a default set of permitted effects, declared per module. It is one language and one checker; a team can
start at the loosest level and tighten module by module.

| Level | Adds, relative to the level above | Typical code |
|---|---|---|
| `kernel` | only the floating-point relaxations | numerical kernels, codecs |
| `realtime` | calls to imported interfaces declared non-blocking, non-allocating and bounded | matching, market-data handling, control loops |
| `service` | allocation, locks, blocking, any imported interface, unbounded loops | business logic, storage, network services |
| `app` | aborting | tools, glue |

Stricter code may only call code whose effects fit its own level. Freedom from out-of-bounds access, overflow and
data races holds at every level.

## What Taut does not claim

- It does not claim to be faster than hand-written C++. The aim is to match it while adding guarantees.
- It is not a memory-safe successor to C++ and does not make the C++ around a component any safer. In a mixed
  program, a wild pointer on the host side can still corrupt a component's memory; that assumption is recorded in
  the trust manifest.
- Static cost bounds and floating-point round-off bounds are not part of the core language. The techniques that
  exist today do not scale far enough.
- Vectorisation is produced by the Taut compiler itself rather than left to a downstream optimiser, but that is an
  implementation choice, not a headline.

## Related work

Each ingredient has precedent, and the design borrows from it deliberately: Wuffs (hermetic libraries with
compile-time bounds and overflow proofs, emitted as C), Futhark and Dex (sizes in types), SPARK and Ada (proof of
absence of run-time errors, restricted profiles), Hylo (mutable value semantics), Koka (effect types), Rust
(ownership), ISPC (vectorisation as a language semantics rather than an optimiser hint), OxCaml (`zero_alloc` and
modes), and Clang's function-effect attributes. What Taut tries to do is combine them in one language whose
components drop into existing C and C++ systems.

## Roadmap

1. Core calculus and checker: data, sizes and ranges, ownership, effects, levels.
2. C backend and the component boundary; first components taken from market-data decoding, an order book and
   numerical kernels.
3. Proof-carrying low-level code (replacing `unsafe` with machine-checked proofs) and the low-level parts of the
   standard library.
4. Platform hosts (whole programs in Taut), an LLVM backend, self-hosting.

## Name

From *tautology*: a property that passes the checker holds in every execution. And from *taut*: no slack — no
hidden checks, allocations or exceptions.

## License

Apache-2.0. See [LICENSE](LICENSE).
