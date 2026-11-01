# Independent embedded implementations

These are frozen, MIT-licensed source copies. They are maintained on their
own and may receive manually ported fixes or optimizations at arbitrary
intervals. They are not automatically synchronized with the main msiqs
implementation. SNAPSHOT.json records the initial source revisions.
Copy this directory as a complete package; no main-tree source is needed.

## Serial SIQS using GMP

Compile siqs.c, lanczos.c, pr32_iter.c, and squfof126.c, linking GMP and libm.
Keep their headers, ptypes.h, mont64.h, siqs_choose_inc.c, and
siqs_sieve_kernel_inc.c alongside them. The implementation includes are not
separate compilation units. No build-mode defines are required.

This edition has no pthreads, journal interfaces or storage, 3LP, or timing
instrumentation. It retains the main implementation's factoring range,
policy bands, recovery profiles, dense solver and serial block Lanczos.

The host supplies one reentrant callback, declared in siqs_dep.h:

```c
#include <gmp.h>
int siqs_is_prob_prime(const mpz_t n) {
  return mpz_probab_prime_p(n, 25);
}
```

Call gmp_siqs(n, &count, 0, verbosity), with a positive input and a non-NULL
count pointer. The input is unchanged. The returned mpz_t array is a verified
multiplicative partition, not necessarily a complete prime factorization.
Release it with gmp_siqs_free(values, count); do not clear elements first.
Allocation/invariant errors terminate through ptypes.h's fatal handler.
Hosts with an existing compatible portability/error header may use it in
place of this package's ptypes.h; MPU-GMP does that to retain Perl exceptions.

A host with main() and the callback can be compiled from the repository root:

```sh
cc -std=c99 -O3 -falign-loops=32 -Iembedded -o my-factor my-factor.c embedded/siqs.c embedded/lanczos.c embedded/pr32_iter.c embedded/squfof126.c -lgmp -lm
```

## TinySIQS128 without GMP

Compile tinysiqs128.c and link libm. Keep tinysiqs128.h, util_128.c, ptypes.h,
and LICENSE with it. util_128.c is included by tinysiqs128.c and must not be
compiled separately. It contains native 128-bit arithmetic and BPSW helpers.
There is no GMP dependency, prime-cache initialization, or external primality
callback. Compiler support for unsigned 128-bit integers is required;
otherwise the public declarations and implementation are excluded.

tinysiqs128(n) returns a proper divisor on success, n for a probable prime,
or zero on failure. Zero and one return themselves. A returned divisor is
not necessarily prime. tinysiqs128_composite(n) skips primality testing;
call it after the host has established compositeness.

## Check the snapshots

From the repository root, run make check-embedded. This builds each edition
independently, checks native primality against GMP, and also links and runs
TinySIQS128 without GMP. The tests require GMP for their reference checks;
TinySIQS128 itself does not.
