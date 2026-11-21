/* Arithmetic-domain, acceptance, and pre-resieve policy regression checks. */
static void tlp_policy_test_context(siqs_ctx_t *ctx, siqs_factor_array_t *result,
                                   const mpz_t n, uint64_t limit,
                                   uint64_t r2, uint32_t cutoff) {
  test_context(ctx, result, n);
  siqs_tlp_policy_clear(ctx);
  ctx->params.large_prime_bound = limit;
  ctx->params.smooth_bound = r2;
  ctx->params.tlp_cutoff = cutoff;
  siqs_mpz_set_u64(ctx->tlp_upper, limit);
  mpz_mul_ui(ctx->tlp_upper, ctx->tlp_upper, 121);
  ctx->params.tlp_smooth_bound = mpz_get_d(ctx->tlp_upper);
  siqs_tlp_policy_init(ctx);
}

static void tlp_policy_integer_bounds(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, q;
  mpz_init_set_ui(n, 10403); mpz_init(q);
  tlp_policy_test_context(&ctx, &result, n, 40, 1000, 17);
  assert(mpz_cmp_ui(ctx.tlp_policy->triple_min, 2197) == 0);
  assert(mpz_cmp_ui(ctx.tlp_policy->gap_lower, 1000) == 0);
  siqs_tlp_policy_clear(&ctx);
  /* Q can exceed UINT32_MAX and both derived bounds can exceed uint64. */
  ctx.largest_fb_prime = UINT32_C(4294967291);
  ctx.params.large_prime_bound = UINT64_C(1) << 48;
  ctx.params.smooth_bound = UINT64_MAX;
  ctx.params.tlp_cutoff = UINT32_MAX;
  siqs_tlp_policy_init(&ctx);
  mpz_set_ui(q, ctx.largest_fb_prime); mpz_nextprime(q, q);
  assert(mpz_cmp_ui(q, UINT32_MAX) > 0);
  mpz_pow_ui(q, q, 3);
  assert(mpz_cmp(q, ctx.tlp_policy->triple_min) == 0);
  assert(mpz_sizeinbase(ctx.tlp_policy->gap_lower, 2) > 64);
  siqs_tlp_policy_clear(&ctx);
  ctx.params.max_large_primes = 2;
  siqs_tlp_policy_init(&ctx);
  assert(ctx.tlp_policy == NULL);
  release_context(&ctx, &result);
  mpz_clears(n, q, NULL);
}

static void tlp_policy_pair_recovery(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, rest, y;
  uint64_t first, second;
  mpz_init_set_ui(n, 898); mpz_init_set_ui(rest, 29 * 31); mpz_init_set_ui(y, 1);
  tlp_policy_test_context(&ctx, &result, n, 40, 1000, 17);
  assert(siqs_resolve_cofactor_outcome(&ctx, rest, &first, &second)
         == SIQS_COFACTOR_RESOLVED && first == 29 && second == 31);
  /* Both labels exceed U, so the previous screen necessarily missed this. */
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_r2_attempts == 1 && ctx.tlp_r2_resolved == 1);
  assert(ctx.tlp_candidates == 0 && ctx.accepted_two_lp == 1);
  assert(ctx.full_count == 1);
  siqs_verify_full_relation(&ctx, ctx.full[0]);
  release_context(&ctx, &result);

  mpz_set_ui(n, 840); mpz_set_ui(rest, 29 * 29);
  tlp_policy_test_context(&ctx, &result, n, 40, 1000, 17);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.accepted_two_lp == 1 && ctx.split_square == 1);
  assert(ctx.full[0]->nfactors == 1 && siqs_lp_archive_exponent(ctx.full[0], 0) == 2);
  siqs_verify_full_relation(&ctx, ctx.full[0]);
  release_context(&ctx, &result);

  mpz_set_ui(n, 10403); mpz_set_ui(rest, 13 * 47);
  tlp_policy_test_context(&ctx, &result, n, 40, 1000, 17);
  assert(siqs_resolve_cofactor_outcome(&ctx, rest, &first, &second)
         == SIQS_COFACTOR_REJECTED);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_r2_rejected == 1 && ctx.tlp_candidates == 0);
  release_context(&ctx, &result);

  /* A tighter future R3 must be checked before an otherwise eligible route. */
  mpz_set_ui(rest, 29 * 31);
  tlp_policy_test_context(&ctx, &result, n, 40, 1000, 17);
  mpz_set_ui(ctx.tlp_upper, 800);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_r2_attempts == 0 && ctx.tlp_candidates == 0);
  release_context(&ctx, &result);
  mpz_clears(n, rest, y, NULL);
}

static void tlp_policy_gap_and_triples(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, rest, y;
  uint64_t before;
  mpz_init_set_ui(n, 2196); mpz_init_set_ui(rest, 2197); mpz_init_set_ui(y, 1);
  tlp_policy_test_context(&ctx, &result, n, 40, 680, 17);
  ctx.buffer_relations = 1;
  mpz_set_ui(rest, 907); /* Strictly in the policy gap, with no FB factor. */
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_gap_rejected == 1U);
  before = ctx.tlp_gap_rejected;
  mpz_set(rest, ctx.tlp_policy->gap_lower);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_gap_rejected == before); /* Lower endpoint is not in gap. */
  mpz_set(rest, ctx.tlp_policy->triple_min);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_gap_rejected == before && ctx.raw_count != 0);
  ctx.buffer_relations = 0;
  release_context(&ctx, &result);

  /* Even R2>=Q^3 must not send a repeated-prime triple to the pair splitter. */
  mpz_set_ui(rest, 13 * 13 * 13);
  tlp_policy_test_context(&ctx, &result, n, 40, 3000, 17);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp_r2_attempts == 0 && ctx.tlp_candidates == 1);
  siqs_tlp_flush(&ctx);
  assert(ctx.accepted_three_lp == 1 && ctx.full_count == 1);
  siqs_verify_full_relation(&ctx, ctx.full[0]);
  release_context(&ctx, &result);
  mpz_clears(n, rest, y, NULL);
}

static void tlp_policy_filter_saturation(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  siqs_poly_t poly;
  mpz_t n;
  mpz_init_set_ui(n, 10403);
  tlp_policy_test_context(&ctx, &result, n, 20, 260, 13);
  memset(&poly, 0, sizeof(poly));
  mpz_inits(poly.DA, poly.B, poly.C, NULL);
  ctx.params.half_interval = 0; ctx.sieve_length = 1;
  ctx.params.sieve_start = 0; ctx.params.log_scale = 1.0;
  ctx.params.sieve_raw_scores = 1; ctx.params.sieve_saturating = 0;
  ctx.candidates = siqs_calloc(1, sizeof(*ctx.candidates)); ctx.candidate_alloc = 1;
  ctx.candidate_at = siqs_calloc(1, sizeof(*ctx.candidate_at));
  ctx.candidates[0].x = 0; ctx.candidates[0].sieve_score = 0;
  ctx.candidate_count = 1; ctx.candidate_at[0] = 1;
  mpz_set_ui(poly.C, 700);
  siqs_filter_candidates(&ctx, &poly);
  assert(ctx.candidate_count == 1);
  /* A zero polynomial must retain the existing immediate-factor path. */
  mpz_set_ui(poly.C, 0);
  ctx.candidate_count = 1; ctx.candidate_at[0] = 1;
  siqs_filter_candidates(&ctx, &poly);
  assert(ctx.candidate_count == 1);
  /* Saturation is only a lower score bound, even if its nominal prediction
   * lands in the rejected interval. 700*2^368/exp(255) is about 824. */
  mpz_set_ui(poly.C, 700); mpz_mul_2exp(poly.C, poly.C, 368);
  ctx.params.sieve_saturating = 1;
  ctx.candidates[0].sieve_score = SIQS_SIEVE_SCORE_MAX;
  ctx.candidate_count = 1; ctx.candidate_at[0] = 1;
  siqs_filter_candidates(&ctx, &poly);
  assert(ctx.candidate_count == 1);
  release_context(&ctx, &result);
  mpz_clears(poly.DA, poly.B, poly.C, n, NULL);
}

static void tlp_policy_kernels(void) {
  tlp_policy_integer_bounds();
  tlp_policy_pair_recovery();
  tlp_policy_gap_and_triples();
  tlp_policy_filter_saturation();
  puts("{\"guarded_pair_recovery\":true,\"exact_gap_endpoints\":true,\"triple_route_guard\":true,\"wide_policy_bounds\":true,\"candidate_filter_and_saturation\":true}");
}
