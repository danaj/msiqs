#ifndef MSIQS_EMBEDDED_DEP_H
#define MSIQS_EMBEDDED_DEP_H
#include <gmp.h>
/* Host-provided, reentrant primality test. Nonzero means prime/probably prime.
 * Do not modify n. A GMP example is provided in README.md. */
extern int siqs_is_prob_prime(const mpz_t n);
#endif
