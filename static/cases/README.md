# Three real cases

    cases/fetch.sh                  # the three projects at pinned commits, into cases/src/
    python3 cases/run.py            # writes cases/out/<case>.{txt,json,effects.json,why.txt}

The projects are fetched, compiled to LLVM bitcode together with a small driver, and analysed. Nothing of
them is executed. A driver (`drivers/<case>/main.cpp`) wraps one documented operation per entry function,
with opaque arguments, so that the entry's call tree is what the operation compiles to.

| case | project, revision | what the project says | what the analysis says |
|---|---|---|---|
| `concurrentqueue` | cameron314/concurrentqueue v1.0.4 (6dd38b8a) | `try_enqueue` "will never allocate memory" (README.md:277) | without a producer token: `alloc may`, `lock may` through `get_or_add_implicit_producer`; with a token: `never`, evidence `direct` |
| `liquibook` | enewhuis/liquibook 2.0.0 (e1970483) | "modern, high-performance techniques" | `callback_now` calls `operator new` at order_book.h:1049 on a likely path; fixed upstream one commit after the tag |
| `tcpshm` | MengRao/tcpshm master (f8258162) | "Non-blocking", "No C++ exceptions", "No use of mutex or atomic operations" | `PollShm` path: `never` for every class, evidence `own` |

The results quoted in the top-level README were produced with `python3 cases/run.py -- --gcc-toolchain /usr`
on Ubuntu 24.04 (system libstdc++ 13, glibc 2.39). The concurrentqueue case defines
`MOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED`: Clang identifies itself as GCC 4.2, for which the header turns
its `thread_local` support off, and the define gives the configuration a GCC build has.

These are observations about what the code can do, checked by hand against the cited source lines. They
are not bug reports, and none of the projects has been contacted.
