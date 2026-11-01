#ifndef MPU_SIQS_DEP_H
#define MPU_SIQS_DEP_H

#include <gmp.h>
#include "ptypes.h"

/* Host adapters for SIQS.  A standalone C embedding supplies this definition
 * (msiqs.c contains the reference implementations).
 * Prime iteration is internal and requires no host-managed cache lifetime.
 * For PSIQS/concurrent C calls, use reentrant adapters, caller-local
 * temporaries, and immutable or synchronized configuration; don't enter
 * non-reentrant host callbacks.
 * See tools/README-siqs-embedding.txt for the complete host contract.
 * No host-managed prime-cache lifetime is required. */


/* Nonzero means prime/probably prime; do not modify n. */
extern int siqs_is_prob_prime(const mpz_t n);


#endif
