# Independent embedded implementations

These are frozen, MIT-licensed source copies.  They are maintained on their
own and may receive manually ported fixes or optimizations at arbitrary
intervals.  They are not automatically synchronized with the main msiqs
implementation.
Copy this directory as a complete package; no main-tree source is needed.

It is also possible to embed the main project's sources, which would give
you all features.  The versions given here remove a lot of features that
are not often needed for other projects, and offer a simpler and lighter
solution.


## What these give

tinysiqs128.c gives you fast 1- to 128-bit factoring, no observed failures
over the range.  It does not use GMP but relies on the compiler having an
unsigned 128-bit type.

siqs.c gives you very fast 1- to 512-bit factoring (tuned only to 430 bits).
No observed failures over the tuned range (1 to 430 bits), multiple factors
can be returned.  It uses GMP, is serial only, no journaling, no debug
timing, and no 3LP mode.

lanczos.c gives you both block Lanczos and dense GF(2) solvers.  They can
be easily used in other projects.  If you want pthreads support, it is
very easy to use the main project code with `planczos_inc.c` added.


## Files needed from this directory

tinysiqs128.c:  tinysiqs128.[ch], ptypes.h, util\_128.c, LICENSE.

siqs.c:  siqs\*.[ch], mont64.h, ptypes.h, pr32\_iter.[ch], tinyecm64.[ch],
         lanczos.[ch], LICENSE.

lanczos.c:  lanczos.[ch], ptypes.h, LICENSE.


## Source versions

These identify the last main-project work incorporated into each edition.
Commit identifiers refer to the preserved history in this repository.

- `tinysiqs128.c`: SIQS optimizations imported on 2026-10-09 (`5423b1e`), plus the 2026-10-22 portability fix (`57f675f`).
- `siqs.c`: adapted from main msiqs on 2026-11-02 (`324bd2f`), with TinyECM64 cofactor splitting ported on 2026-11-04.
- `lanczos.c`: adapted from main msiqs on 2026-11-02 (`324bd2f`).


## Serial SIQS using GMP

Compile siqs.c, lanczos.c, pr32\_iter.c, and tinyecm64.c, linking GMP and libm.
Keep their headers, ptypes.h, mont64.h, siqs\_choose\_inc.c, and
siqs\_sieve\_kernel\_inc.c alongside them. The implementation includes are not
separate compilation units. No build-mode defines are required.

This edition has no pthreads, journal interfaces or storage, 3LP, or timing
instrumentation. It retains the main implementation's factoring range,
policy bands, recovery profiles, dense solver and serial block Lanczos.

GMP 6.2 or newer is recommended for its BPSW primality test.

Call `gmp_siqs(n, &count, 0, verbosity)`, with a positive input and a non-NULL
uint32\_t count pointer. The input is unchanged. The returned mpz\_t array
is a verified multiplicative partition, not necessarily a complete prime
factorization.  Release it with `gmp_siqs_free(values, count)`; do not clear
elements first.
Allocation/invariant errors terminate through ptypes.h's fatal handler.
Hosts with an existing compatible portability/error header may use it in
place of this package's ptypes.h -- e.g. MPU-GMP does that to retain Perl
exceptions.

A host with main() can be compiled from the repository root:

```sh
cc -O3 -falign-loops=32 -Iembedded -o my-factor my-factor.c embedded/siqs.c embedded/lanczos.c embedded/pr32_iter.c embedded/tinyecm64.c -lgmp -lm
```


## TinySIQS128 without GMP

Compile tinysiqs128.c and link libm. Keep tinysiqs128.h, util\_128.c, ptypes.h,
and LICENSE with it. util\_128.c is included by tinysiqs128.c and must not be
compiled separately. It contains native 128-bit arithmetic and BPSW helpers.
There is no GMP dependency, prime-cache initialization, or external primality
callback. Compiler support for unsigned 128-bit integers is required;
otherwise the public declarations and implementation are excluded.

`tinysiqs128(n)` returns a proper divisor on success, n for a probable prime,
or zero on failure. Zero and one return themselves. A returned divisor is
not necessarily prime. `tinysiqs128_composite(n)` skips primality testing;
call it after the host has established compositeness.


## GF(2) matrix solvers

Compile lanczos.c and include lanczos.h; no GMP or libm is needed.  The solvers
leave the input matrix unchanged and return dependency arrays released with
`free`.  The optional reducer modifies the columns and frees discarded data.
See [Using the GF(2) matrix solvers](../docs/LANCZOS-USE.md) for the matrix
format, solver calls, result handling, and parallel support in the main project.


## Testing the snapshots

From the repository root, run make check-embedded. This builds each edition
independently, checks native primality against GMP, and also links and runs
TinySIQS128 without GMP. The tests require GMP for their reference checks;
TinySIQS128 itself does not.
