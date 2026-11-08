/* Private LP-combiner checks: finite-space kernels, packed archive, cleanup,
 * LP divisors of the input and dependency scratch growth after collection. */
#define STANDALONE
#define SIQS_DEBUG
#include <assert.h>
#include "../siqs.c"
#include "siqs-check-lp.inc.c"

static void archive_context(siqs_ctx_t *ctx, siqs_factor_array_t *result,
                            mpz_t n) {
  siqs_factor_array_init(result, n);
  siqs_ctx_init(ctx, n, n, result, NULL, 0);
  ctx->params.fb_size = 2;
  ctx->params.max_large_primes = 2;
  ctx->params.large_prime_bound = 1000;
  ctx->fb = siqs_calloc(2, sizeof(*ctx->fb));
  ctx->fb[0].p = 2; ctx->fb[1].p = 3;
  ctx->factor_row_alloc = 3;
  ctx->factor_counts = siqs_calloc(3, sizeof(*ctx->factor_counts));
  ctx->factor_touched = siqs_malloc(3 * sizeof(*ctx->factor_touched));
  siqs_hashset_init(&ctx->relation_hashes, 1024);
  siqs_lp_combiner_init(ctx);
}

static void archive_release(siqs_ctx_t *ctx, siqs_factor_array_t *result) {
  siqs_ctx_clear(ctx);
  gmp_siqs_free(result->values, result->count);
  free(result->primality);
}

static void archive_boundaries(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  uint32_t i, exponents[4] = {30, 32, 64, 2};
  mpz_t n, y;
  assert(siqs_lp_archive_factor_fits(SIQS_LP_ARCHIVE_ROW_MASK, 31));
  assert(!siqs_lp_archive_factor_fits(SIQS_LP_ARCHIVE_ROW_MASK + 1U, 31));
  assert(!siqs_lp_archive_factor_fits(0, 32));
  assert(!siqs_lp_archive_factor_fits(UINT32_MAX, UINT32_MAX));
  mpz_init_set_ui(n, 10403); mpz_init(y);
  archive_context(&ctx, &result, n);
  for (i = 0; i < 4; ++i) {
    siqs_factor_t f;
    siqs_full_relation_t *r;
    uint64_t lp = i == 3 ? 53 : 1;
    f.row = 1; f.exponent = exponents[i];
    mpz_set_ui(y, 1); mpz_mul_2exp(y, y, f.exponent / 2);
    mpz_mul_ui(y, y, (unsigned long)lp); mpz_mod(y, y, n);
    siqs_accept_raw_relation(&ctx, siqs_raw_relation_new(&ctx, y, &f, 1, lp, lp));
    r = ctx.full[i];
    assert(r->lp_packed == (f.exponent <= 31));
    assert(siqs_lp_archive_row(r, 0) == 1);
    assert(siqs_lp_archive_exponent(r, 0) == f.exponent);
    if (lp > 1) {
      assert(r->nfactors == 2);
      assert(siqs_lp_archive_row(r, 1) == 3);
      assert(siqs_lp_archive_exponent(r, 1) == 2);
    } else assert(r->nfactors == 1);
    siqs_verify_full_relation(&ctx, r);
  }
  assert(siqs_relation_count(&ctx) == 4 && siqs_partial_count(&ctx) == 0);
  siqs_lp_prepare_dependency(&ctx);
  assert(ctx.factor_row_alloc == 4);
  assert(siqs_touch_factor(&ctx, 3, 2));
  siqs_reset_touched_factors(&ctx);
  (void)siqs_lp_combiner_label(&ctx, 59);
  siqs_lp_prepare_dependency(&ctx);
  assert(ctx.factor_row_alloc == 5 && ctx.factor_counts[4] == 0);
  archive_release(&ctx, &result);
  mpz_clear(n); mpz_clear(y);
}

static void archive_input_divisor(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  siqs_factor_t f;
  mpz_t n, y;
  mpz_init_set_ui(n, 10403); mpz_init_set_ui(y, 1616);
  archive_context(&ctx, &result, n);
  f.row = 2; f.exponent = 1;
  /* 1616^2 == 3*101 mod (101*103). */
  siqs_accept_raw_relation(&ctx, siqs_raw_relation_new(&ctx, y, &f, 1, 1, 101));
  assert(ctx.factor_found && result.count == 2);
  assert(siqs_all_factors_prime(&result));
  archive_release(&ctx, &result);
  mpz_clear(n); mpz_clear(y);
}

static void bids_keeps_legacy_collector(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, q;
  mpz_init_set_ui(n, 1); mpz_mul_2exp(n, n, 89); mpz_sub_ui(n, n, 1);
  mpz_init_set_ui(q, 1); mpz_mul_2exp(q, q, 107); mpz_sub_ui(q, q, 1);
  mpz_mul(n, n, q);
  siqs_factor_array_init(&result, n);
  siqs_ctx_init(&ctx, n, n, &result, NULL, 0);
  ctx.params.fb_size = 64;
  ctx.params.max_large_primes = 2;
  ctx.use_bids = 1;
  assert(siqs_ctx_allocate(&ctx));
  assert(ctx.lp_combiner == NULL && ctx.graph.vertex_count == 1);
  archive_release(&ctx, &result);
  mpz_clear(n); mpz_clear(q);
}

int main(void) {
  lp_combiner_kernels();
  archive_boundaries();
  archive_input_divisor();
  bids_keeps_legacy_collector();
  puts("LP checks: all enabled checks passed.");
  return 0;
}
