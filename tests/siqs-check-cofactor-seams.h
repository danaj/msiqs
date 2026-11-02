/* Test-only bounded-splitter misses. Include after the driver and
 * before siqs.c; undefine the interposition immediately afterward. Flags
 * stay immutable while any concurrent cofactor checks are running. */
#include "../squfof126.h"
static int cofactor_miss_squfof;
static int cofactor_miss_siqs, cofactor_trace;
static unsigned int cofactor_squfof_calls, cofactor_prime_calls;
static int check_cofactor_squfof(const mpz_t n, mpz_t f, uint64_t rounds) {
  if (cofactor_trace) cofactor_squfof_calls++;
  return cofactor_miss_squfof ? 0 : squfof126(n, f, rounds);
}
/* Capture the real GMP call before interposing the private SIQS helper.
 * Explicit reference checks after siqs.c also use this real entry point. */
static int check_real_prob_prime(const mpz_t n, int reps) {
  return mpz_probab_prime_p(n, reps);
}
/* A false "prime" answer makes the inner SIQS return its unsplit partition.
 * The resolver's native outer primality/acceptance checks are not interposed. */
static int check_cofactor_prime(const mpz_t n, int reps) {
  if (cofactor_trace) cofactor_prime_calls++;
  return cofactor_miss_siqs ? 1 : check_real_prob_prime(n, reps);
}
#define squfof126 check_cofactor_squfof
#undef mpz_probab_prime_p
#define mpz_probab_prime_p check_cofactor_prime
