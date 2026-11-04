/* Test-only bounded-splitter misses. Include after the driver and
 * before siqs.c; undefine the interposition immediately afterward. Flags
 * stay immutable while any concurrent cofactor checks are running. */
#include "../tinyecm64.h"
static int cofactor_miss_ecm;
static int cofactor_miss_siqs, cofactor_trace;
static unsigned int cofactor_ecm_calls, cofactor_prime_calls;
static uint64_t cofactor_ecm_input, cofactor_ecm_seed;
static int cofactor_ecm_try_small;
static uint64_t check_cofactor_ecm(uint64_t n, int try_small, uint64_t *state) {
  if (cofactor_trace) {
    cofactor_ecm_calls++;
    cofactor_ecm_input = n;
    cofactor_ecm_seed = *state;
    cofactor_ecm_try_small = try_small;
  }
  return cofactor_miss_ecm ? 0 : tinyecm64(n, try_small, state);
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
#define tinyecm64 check_cofactor_ecm
#undef mpz_probab_prime_p
#define mpz_probab_prime_p check_cofactor_prime
