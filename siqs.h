#ifndef MPU_SIQS_H
#define MPU_SIQS_H

#include <gmp.h>
#include "ptypes.h"


/* Lowest post-trial cofactor size accepted by the SIQS stage. */
#define MPU_SIQS_MIN_BITS   1U
/* Arithmetic and policy-coverage limit, not a performance guarantee.
 * We have currently verified good performance through 400 bits. */
#define MPU_SIQS_MAX_BITS 512U

/* Parallel collection is optional; small/inline-solving policies stay serial. */
#define PSIQS_MAX_THREADS 256U
#ifndef PSIQS_SERIAL_MAX_BITS
# define PSIQS_SERIAL_MAX_BITS 64U
#endif

/* Host lifetime for the entry points:
 *   - No prime-cache startup/shutdown is required.  The independent pr32_iter
 *     uses immutable base tables and iterator-owned segment buffers.
 *   - Calls have independent writable state and may run concurrently with
 *     separate output storage. Inputs may be shared read-only, but must not
 *     be mutated or cleared during calls. */

#define SIQS_OPTION_3LP 1U
#define SIQS_OPTION_2LP 2U

/* n must be positive.  Return an allocated multiplicative partition of n.
 * Partition elements are not necessarily prime, and any partial splitting is
 * retained.  If no split is found, the sole element is n.  The SIQS stage is
 * attempted only when the post-trial cofactor is between MPU_SIQS_MIN_BITS
 * and MPU_SIQS_MAX_BITS, inclusive.
 *
 * n is not modified.  The caller owns all returned mpz_t values and the array.
 * Release both with gmp_siqs_free, without separately clearing/freeing its
 * elements first.
 *
 * There is no complete-factorization flag; callers must check the partition.
 *
 * nfactors must be non-NULL and receives the array length, including repeated
 * factors.
 *
 * trial_start is the first candidate not already checked for small factors;
 * use 0 for ordinary pretests.
 *
 * verbose 0 is quiet, 1 prints setup/final summaries, 2 adds periodic progress.
 * Level 3 or higher enables diagnostics when applicable.
 * Negative values become 0.
 *
 * nthreads 0 and 1 select serial execution.  Higher values request up to that
 * many workers per call.  nthreads higher than PSIQS_MAX_THREADS will error.
 * In builds without PSIQS defined, values above 1 will error.
 *
 * Large-prime policy selection uses the post-trial cofactor size.
 * With flags == 0, SIQS chooses the default policy for that size.
 * The current tables select 3LP by default from 361 bits.
 * SIQS_OPTION_2LP selects the ordinary policy, which allows up to two
 * large primes. SIQS_OPTION_3LP requests 3LP wherever supported
 * (currently from 270 bits) and uses the ordinary policy elsewhere.
 * Unknown flag bits and combining both LP flags are errors.
 *
 * Internal cofactor split always uses quiet, serial, default LP policies.
 *
 * Standalone fatal allocation/invariant errors exit rather than return NULL.
 *
 * Quiet serial call with automatic LP selection
 *   uint32_t nfactors;
 *   mpz_t *factors = gmp_siqs(n, &nfactors, 0, 0, 1, 0);
 *
 * The embedded edition uses a simpler serial API.
 *   mpz_t *factors = gmp_siqs(n, &nfactors, 0, 0);
 * See embedded/README.md for the complete embedded example. */
extern mpz_t *gmp_siqs(const mpz_t n, uint32_t *nfactors, uint32_t trial_start,
                       int verbose, uint32_t nthreads, uint32_t flags);

extern void gmp_siqs_free(mpz_t *factors, uint32_t nfactors);



/* Call-owned journal session.  One coordinator owns all I/O.
 * A session belongs to one original input, but can record several smaller
 * cofactor calls and recovery policies.  Close only after every call returns.
 * Existing journals are read once, before any new frames are appended. */
typedef struct siqs_journal_t siqs_journal_t;

/* n may be NULL to obtain the input from an existing journal.  In that mode
 * a missing file is an error; no new journal is created. */
extern siqs_journal_t *gmp_siqs_journal_open(const char *path, mpz_srcptr n);

extern void gmp_siqs_journal_input(const siqs_journal_t *journal, mpz_t n);

/* Worker counts and flags have the same meaning as in gmp_siqs. */
extern mpz_t *gmp_siqs_journal_factor(siqs_journal_t *journal, const mpz_t n,
    uint32_t *nfactors, uint32_t trial_start, int verbose, uint32_t nthreads,
    uint32_t flags);

/* Record the front end's final partition after processing every cofactor.
 * An interruption or a mere close does not imply that the job finished. */
extern void gmp_siqs_journal_finish(siqs_journal_t *journal, mpz_t *values,
                                    size_t count, int complete);

extern void gmp_siqs_journal_close(siqs_journal_t *journal);

#endif
