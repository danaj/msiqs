#define STANDALONE
#define SIQS_DEBUG
#include <assert.h>
#ifndef SIQS_TEST_SOURCE
#define SIQS_TEST_SOURCE "../../siqs.c"
#endif
#include SIQS_TEST_SOURCE

int main(int argc, char **argv) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  siqs_poly_t poly;
  siqs_journal_t *j;
  siqs_factor_t f = {1, 2};
  uint64_t families = 0, polynomials = 0;
  mpz_t n, p, q, y, root, inverse, exponent;
  int reading;
  assert(argc == 2);
  mpz_init_set_ui(p, 65537);
  mpz_init_set_ui(q, 1); mpz_mul_2exp(q, q, 127); mpz_sub_ui(q, q, 1);
  mpz_init(n); mpz_mul(n, p, q);
  mpz_inits(y, root, inverse, exponent, NULL);
  j = gmp_siqs_journal_open(argv[1], n); reading = j->reading;
  siqs_factor_array_init(&result, n); result.journal = j;
  siqs_ctx_init(&ctx, n, n, &result, NULL, 0);
  ctx.params.max_large_primes = 2;
  ctx.params.fb_size = 64;
  assert(siqs_ctx_allocate(&ctx));
  ctx.params.large_prime_bound = 1000000;
  ctx.params.smooth_bound = (uint64_t)ctx.largest_fb_prime * 1000000;
  ctx.params.sieve_hit_bound = 2.0 * (double)ctx.params.smooth_bound;
  assert(ctx.largest_fb_prime < 65537 && ctx.params.large_prime_bound >= 65537);
  /* Produce the historical failed-inverse case, then replay it through
   * the new LP kernel. The legacy collector remains available for BIDS. */
  if (!reading) {
    siqs_lp_combiner_clear(&ctx);
    siqs_one_lp_init(&ctx.one_lp);
    siqs_graph_init(&ctx.graph);
  }
  siqs_poly_init(&ctx, &poly);
  ctx.matrix_next_target = ctx.params.target_relations;
  assert(!sj_attach(&ctx, &poly, &families, &polynomials));
  if (!reading) {
    sj_begin_attempt(&ctx);
    assert(mpz_jacobi(p, q) == 1);
    mpz_add_ui(exponent, q, 1); mpz_fdiv_q_2exp(exponent, exponent, 2);
    mpz_powm(root, p, exponent, q);
    assert(mpz_invert(inverse, p, q));
    mpz_mul(y, root, inverse); mpz_mod(y, y, q); mpz_mul(y, y, p);
    siqs_accept_raw_relation(&ctx, siqs_raw_relation_new(&ctx, y, NULL, 0, 1, 65537));
    mpz_mul_ui(y, y, 2); mpz_mod(y, y, n);
    siqs_accept_raw_relation(&ctx, siqs_raw_relation_new(&ctx, y, &f, 1, 1, 65537));
    ctx.total_candidates = 2;
    sj_flush(&ctx);
  }
  assert(ctx.factor_found && result.count == 2 && siqs_all_factors_prime(&result));
  puts("PASS LP-divisor journal replay");
  siqs_poly_clear(&ctx, &poly);
  siqs_ctx_clear(&ctx);
  gmp_siqs_journal_close(j);
  gmp_siqs_free(result.values, result.count); free(result.primality);
  mpz_clears(n, p, q, y, root, inverse, exponent, NULL);
  return 0;
}
