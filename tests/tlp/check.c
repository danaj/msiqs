/* Private-interface correctness tests; not linked into the implementation. */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define SIQS_DEBUG 1
/* Assertions also run fixture setup and worker synchronization. */
#ifdef NDEBUG
# undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <gmp.h>
#include "../../tinyecm64.h"
#ifdef PSIQS
#include <pthread.h>
static pthread_mutex_t test_counter_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static uint64_t host_prime_calls;
static uint64_t test_ecm_calls;
static unsigned test_force_ecm_miss, test_ecm_forced_misses;
static void test_increment(uint64_t *counter) {
#ifdef PSIQS
  pthread_mutex_lock(&test_counter_lock);
#endif
  ++*counter;
#ifdef PSIQS
  pthread_mutex_unlock(&test_counter_lock);
#endif
}
static uint64_t test_tinyecm64(uint64_t n, int try_small) {
  test_increment(&test_ecm_calls);
  if (test_force_ecm_miss) {
    --test_force_ecm_miss; ++test_ecm_forced_misses;
    return 0;
  }
  return tinyecm64(n, try_small);
}
static int test_prob_prime(const mpz_t n, int rounds) {
  test_increment(&host_prime_calls);
  return mpz_probab_prime_p(n, rounds);
}
/* Count the static GMP helper without changing the production interface. */
#pragma push_macro("mpz_probab_prime_p")
#undef mpz_probab_prime_p
#define mpz_probab_prime_p test_prob_prime
#define tinyecm64 test_tinyecm64
#include "../../siqs.c"
#undef tinyecm64
#pragma pop_macro("mpz_probab_prime_p")

#ifndef SIQS_NO_JOURNAL
/* Keep ordinary checkpoint behavior, while allowing a TLP batch to overlap
 * sieve work. Sparse TLP output must still reach a timed checkpoint. */
static void journal_checkpoint_policy(void) {
  siqs_ctx_t ctx; siqs_journal_t j;
  memset(&ctx, 0, sizeof(ctx)); memset(&j, 0, sizeof(j)); ctx.journal = &j;
  ctx.params.max_large_primes = 2; j.events = 255;
  assert(!sj_batch_due(&ctx)); j.events = 256; assert(sj_batch_due(&ctx));
  j.events = 0; j.batch.size = SIQS_JOURNAL_BATCH_BYTES - 1;
  assert(!sj_batch_due(&ctx));
  j.batch.size++; assert(sj_batch_due(&ctx));
  ctx.params.max_large_primes = 3; assert(!sj_batch_due(&ctx));
  j.batch.size = SJ_TLP_BATCH_BYTES - 1; assert(!sj_batch_due(&ctx));
  j.batch.size++; assert(sj_batch_due(&ctx));
  ctx.params.max_large_primes = 3; j.batch.size = 64;
  j.events = 4096; assert(!sj_batch_due(&ctx));
  j.clock_started = 1; j.last_checkpoint = sj_clock_us(&j, 1);
  assert(!sj_batch_due(&ctx));
  assert(j.last_checkpoint >= UINT64_C(20000000));
  j.last_checkpoint -= UINT64_C(10000000); assert(!sj_batch_due(&ctx));
  j.last_checkpoint -= UINT64_C(10000000); assert(sj_batch_due(&ctx));
  j.batch.size = 0; assert(!sj_batch_due(&ctx));
  /* A fresh journal writes INPUT before collection begins. That first write
   * must initialize the checkpoint clock as well as the frame clock. */
  memset(&j, 0, sizeof(j)); j.file = tmpfile(); assert(j.file != NULL);
  j.path = (char *)"checkpoint-clock-test";
  {
    sj_buffer b = {NULL, 0, 0};
    sj_write(&j, SJ_INPUT, 0, &b);
  }
  assert(j.clock_started && j.last_checkpoint == j.mono_start);
  sj_begin_attempt(&ctx);
  j.batch.size = 64; j.events = 4096; assert(!sj_batch_due(&ctx));
  assert(fclose(j.file) == 0);
  puts("{\"ordinary_journal_limit\":true,\"TLP_journal_batch_overlap\":true,\"TLP_journal_time_target\":true,\"fresh_checkpoint_clock\":true}");
}
#endif

/* Sparse 3LP bands must never change the ordinary resolver or conceal an
 * overlap. Check automatic/forced selection independently from resolution,
 * and alternate modes to catch retained selection. */
static void policy_table_kernels(void) {
  uint32_t bits, i;
  assert(MPU_SIQS_MAX_BITS == 512U);
  assert(SIQS_DEFAULT_TLP_MIN_BITS == 361U);
  for (bits = MPU_SIQS_MIN_BITS; bits <= MPU_SIQS_MAX_BITS; ++bits) {
    siqs_policy_t ordinary, selected, again;
    const siqs_policy_band_t *tlp = siqs_tlp_policy_band(bits);
    uint32_t matches = 0;
    siqs_resolve_policy(&ordinary, bits, NULL);
    assert(ordinary.max_large_primes >= 1 && ordinary.max_large_primes <= 2);
    assert(ordinary.fb_scale == 1.0 && ordinary.lp_scale == 1.0 && ordinary.r2_scale == 1.0);
    for (i = 0; i < SIQS_TLP_POLICY_BAND_COUNT; ++i) {
      const siqs_policy_band_t *row = &siqs_tlp_policy_bands[i];
      assert(row->first_bits <= row->last_bits);
      assert(strncmp(row->name, "3lp_", 4) == 0);
      if (bits >= row->first_bits && bits <= row->last_bits) {
        ++matches;
        assert(tlp == row);
      }
    }
    assert(matches <= 1 && (tlp != NULL) == (matches == 1));
    /* Check documented coverage independently of the policy rows. */
    assert((tlp != NULL) == (bits >= 270U));
    assert(siqs_initial_policy_band(bits, SIQS_OPTION_2LP) == NULL);
    assert(siqs_initial_policy_band(bits, SIQS_OPTION_3LP) == tlp);
    assert(siqs_initial_policy_band(bits, 0) == (bits >= 361U ? tlp : NULL));
    siqs_resolve_policy(&selected, bits, tlp);
    if (tlp != NULL) assert(selected.max_large_primes == 3);
    else assert(memcmp(&ordinary, &selected, sizeof(ordinary)) == 0);
    siqs_resolve_policy(&again, bits, NULL);
    assert(memcmp(&ordinary, &again, sizeof(ordinary)) == 0);
  }
  puts("{\"ordinary_policy_coverage\":true,\"sparse_tlp_policies\":true,\"automatic_tlp_boundary\":true,\"forced_lp_modes\":true,\"policy_selection_isolation\":true}");
}

static void test_context(siqs_ctx_t *ctx, siqs_factor_array_t *result,
                          const mpz_t n) {
  const uint32_t primes[] = {2, 3, 5, 7, 11};
  uint32_t i;
  memset(ctx, 0, sizeof(*ctx));
  ctx->original_n = n; ctx->result = result;
  mpz_init_set(ctx->n, n); mpz_init_set(ctx->kn, n);
  mpz_inits(ctx->eval.y, ctx->eval.q, ctx->eval.rest, NULL);
  mpz_init_set_ui(ctx->tlp_upper, 121000);
  ctx->params.bits = (uint32_t)mpz_sizeinbase(n, 2);
  ctx->params.max_large_primes = 3;
  ctx->params.fb_size = 5;
  ctx->params.large_prime_bound = 1000;
  ctx->params.tlp_cutoff = 23;
  ctx->params.tlp_smooth_bound = 121000;
  ctx->params.policy_name = "synthetic_three_lp";
  ctx->fb = (siqs_fb_t *)siqs_calloc(5, sizeof(*ctx->fb));
  for (i = 0; i < 5; ++i) ctx->fb[i].p = primes[i];
  ctx->largest_fb_prime = 11;
  siqs_tlp_policy_init(ctx);
  ctx->factor_row_alloc = 6;
  ctx->factor_counts = (uint32_t *)siqs_calloc(6, sizeof(uint32_t));
  ctx->factor_touched = (uint32_t *)siqs_malloc(6 * sizeof(uint32_t));
  siqs_factor_array_init(result, n);
  siqs_hashset_init(&ctx->relation_hashes, 64);
  siqs_lp_combiner_init(ctx);
  siqs_tlp_init(ctx);
}

static void release_context(siqs_ctx_t *ctx, siqs_factor_array_t *result) {
  siqs_ctx_clear(ctx);
  free(result->primality);
  gmp_siqs_free(result->values, result->count);
}

#ifndef SIQS_NO_JOURNAL
/* The serial TLP queue also stays pending across a periodic checkpoint. */
static void journal_pending_lp(void) {
  siqs_ctx_t ctx; siqs_factor_array_t result; siqs_journal_t j;
  uint64_t polys=0, checkpoint; mpz_t n,rest,y;
  mpz_init_set_ui(n,10403);mpz_init_set_ui(rest,4199);mpz_init_set_ui(y,1);
  test_context(&ctx,&result,n);
  memset(&j,0,sizeof(j));j.file=tmpfile();assert(j.file!=NULL);
  j.path=(char *)"pending-LP-checkpoint";j.lp_bytes=2;
  ctx.journal=&j;ctx.journal_poly_count=&polys;
  ctx.la_rng.state=11;ctx.cofactor_rng.state=13;ctx.poly_rng.state=17;
  siqs_accept_raw_relation(&ctx,siqs_raw_relation_new3(&ctx,y,NULL,0,1,1,1));
  mpz_set_ui(y,991);siqs_resolve_cofactor_3lp(&ctx,rest,y,NULL,0);
  assert(ctx.full_count==1 && ctx.tlp->count==1);
  sj_flush(&ctx);
  assert(j.batch.size==0 && ctx.full_count==1 && ctx.tlp->count==1);
  checkpoint=j.sequence;
  assert(j.last_checkpoint>=UINT64_C(20000000));
  j.last_checkpoint-=UINT64_C(20000000);
  siqs_tlp_flush(&ctx);
  assert(j.sequence==checkpoint+1 && j.batch.size==0 && ctx.full_count==2);
  mpz_sub(y,n,y);siqs_resolve_cofactor_3lp(&ctx,rest,y,NULL,0);
  assert(ctx.tlp->count==1);
  sj_end_attempt(&ctx);
  assert(j.batch.size==0 && ctx.tlp->count==0 && ctx.full_count==3);
  assert(ctx.accepted_smooth==1 && ctx.accepted_three_lp==2);
  ctx.journal=NULL;free(j.batch.data);assert(fclose(j.file)==0);
  release_context(&ctx,&result);mpz_clears(n,rest,y,NULL);
  puts("{\"checkpoint_keeps_LP_queue\":true,\"batch_publication_checks_checkpoint\":true,\"attempt_end_drains_LP\":true}");
}
#endif

static void triples_and_dependency(void) {
  mpz_t n, rest, y;
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  uint64_t before, mask, *nullrows;
  unsigned long nrows, ncols, i;
  la_col_t *columns;
  mpz_init_set_ui(n, 10403); mpz_init_set_ui(rest, 13 * 17 * 19); mpz_init(y);
  test_context(&ctx, &result, n);
  before = host_prime_calls;
  mpz_set_ui(y, 991);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  mpz_set_ui(y, 2948);
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(host_prime_calls == before && ctx.tlp->count == 2 && ctx.full_count == 0);
  siqs_tlp_flush(&ctx);
  assert(ctx.tlp->count == 0 && ctx.tlp_batches == 1 && ctx.tlp_gcd_whole == 2);
  assert(ctx.accepted_three_lp == 2 && ctx.full_count == 2);
  assert(ctx.lp_combiner->label_count == 3);
  for (i = 0; i < ctx.full_count; ++i) siqs_verify_full_relation(&ctx, ctx.full[i]);
  columns = siqs_lp_combiner_matrix(&ctx, &nrows, &ncols);
  assert(ctx.lp_combiner->rank == 1 && nrows == 6 && ncols == 1 && columns[0].weight == 0);
  nullrows = la_dense_nullspace(nrows, ncols, columns, &mask);
  assert(nullrows != NULL && mask != 0);
  assert(siqs_test_dependencies(&ctx, columns, ncols, nullrows, mask));
  assert(result.count == 2 && siqs_all_factors_prime(&result));
  siqs_verify_partition(n, &result);
  for (i = 0; i < ncols; ++i) free(columns[i].data);
  free(columns); free(nullrows);
  release_context(&ctx, &result);
  mpz_clears(n, rest, y, NULL);
}

static void solve_finds_factor_during_drain(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, rest, y;
  mpz_init_set_ui(n, 13 * 29);
  mpz_init_set_ui(rest, 13 * 17 * 19);
  mpz_init_set_ui(y, 78);
  test_context(&ctx, &result, n);
  /* 78^2 is congruent to rest modulo n. The singleton has no LP-free
   * combination, but its label 13 gives a factor as soon as it is accepted. */
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  assert(ctx.tlp->count == 1 && !ctx.factor_found);
  assert(siqs_solve(&ctx));
  assert(ctx.factor_found && result.count == 2 && siqs_relation_count(&ctx) == 0);
  siqs_verify_partition(n, &result);
  release_context(&ctx, &result);
  mpz_clears(n, rest, y, NULL);
}

static void rejects_and_repeated_labels(void) {
  mpz_t n, rest, y, gcd;
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  uint64_t before, labels[3]; uint32_t count;
  mpz_init_set_ui(n, 10403); mpz_init_set_ui(rest, 1009);
  mpz_init_set_ui(y, 1); mpz_init(gcd);
  test_context(&ctx, &result, n);
  before = host_prime_calls;
  siqs_resolve_cofactor_3lp(&ctx, rest, y, NULL, 0);
  siqs_tlp_flush(&ctx);
  assert(ctx.tlp_gcd_one == 1 && ctx.full_count == 0 && host_prime_calls == before);
  mpz_set_ui(rest, 13 * 13 * 17); mpz_set_ui(gcd, 13 * 17);
  assert(siqs_tlp_complete(&ctx, rest, gcd, labels, &count));
  assert(count == 3 && labels[0] == 13 && labels[1] == 13 && labels[2] == 17);
  mpz_set_ui(rest, 13 * 17 * 19 * 23); mpz_set_ui(gcd, 13 * 17);
  assert(!siqs_tlp_complete(&ctx, rest, gcd, labels, &count));
  mpz_set_ui(rest, 13 * 1009); mpz_set_ui(gcd, 13);
  assert(!siqs_tlp_complete(&ctx, rest, gcd, labels, &count));
  /* A repeated LP has exponent 2 in the matrix, retaining the square root. */
  {
    siqs_raw_relation_t *raw;
    unsigned long v;
    mpz_set_ui(rest, 13 * 13 * 17);
    for (v = 1; v < 10403; ++v) if (v * v % 10403 == 2873) break;
    assert(v < 10403);
    {
      mpz_set_ui(y, v);
      raw = siqs_raw_relation_new3(&ctx, y, NULL, 0, 13, 13, 17);
      siqs_accept_raw_relation(&ctx, raw);
      assert(ctx.full_count == 1 && ctx.full[0]->nfactors == 2);
      assert(siqs_lp_archive_exponent(ctx.full[0], 0) == 2);
      siqs_verify_full_relation(&ctx, ctx.full[0]);
    }
  }
  release_context(&ctx, &result);
  mpz_clears(n, rest, y, gcd, NULL);
}

/* The source of a piece does not change its 64-bit splitter or fallback. */
static void tinyecm_completion(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, input, gcd;
  uint64_t labels[3], before;
  uint32_t count;
  unsigned misses;
  mpz_init_set_ui(n, 10403); mpz_init(input); mpz_init(gcd);
  test_context(&ctx, &result, n);
  ctx.params.large_prime_bound = UINT64_C(4294967296);
  /* The quotient is an unknown 62-bit pair; it must use tinyecm64 too. */
  mpz_set_ui(input, 13); mpz_mul_ui(input, input, 2147483647UL);
  mpz_mul_ui(input, input, 2147483629UL); mpz_set_ui(gcd, 13);
  before = test_ecm_calls;
  assert(siqs_tlp_complete(&ctx, input, gcd, labels, &count));
  assert(test_ecm_calls > before && count == 3);
  assert(labels[0] == 13 && labels[1] == 2147483629UL && labels[2] == 2147483647UL);
  /* Squares bypass ECM. */
  mpz_set_ui(input, 13*13*17); mpz_set_ui(gcd, 13*13);
  before = test_ecm_calls;
  assert(siqs_tlp_complete(&ctx, input, gcd, labels, &count));
  assert(test_ecm_calls == before && count == 3);
  assert(labels[0] == 13 && labels[1] == 13 && labels[2] == 17);
  /* A forced ECM miss must still reach quiet SIQS and complete the tuple. */
  mpz_set_ui(input, 13*17*19); mpz_set_ui(gcd, 13*17);
  misses = test_ecm_forced_misses; test_force_ecm_miss = 1;
  assert(siqs_tlp_complete(&ctx, input, gcd, labels, &count));
  assert(test_force_ecm_miss == 0 && test_ecm_forced_misses == misses+1);
  assert(count == 3 && labels[0] == 13 && labels[1] == 17 && labels[2] == 19);
  release_context(&ctx, &result); mpz_clears(n, input, gcd, NULL);
  puts("{\"tinyecm64_completion\":true,\"wide_quotient\":true,\"square_first\":true,\"SIQS_after_ECM_miss\":true}");
}

/* The integration fixture has R3 below 2^64. Exercise quiet SIQS completion
 * of a wider, squarefree known-U product with three admissible LPs. */
static void wide_siqs_completion(void) {
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  mpz_t n, input, prime;
  uint64_t expected[3], labels[3];
  uint32_t count, i;
  mpz_init_set_ui(n, 10403); mpz_init_set_ui(input, 1); mpz_init(prime);
  test_context(&ctx, &result, n);
  ctx.params.large_prime_bound = UINT64_C(1) << 29;
  ctx.params.tlp_cutoff = UINT32_C(1) << 28;
  for (i = 0; i < 3; ++i) {
    mpz_set_ui(prime, 1);
    mpz_mul_2exp(prime, prime, 25U + i);
    mpz_add_ui(prime, prime, 12345);
    mpz_nextprime(prime, prime);
    expected[i] = mpz_get_ui(prime);
    mpz_mul(input, input, prime);
  }
  mpz_set(ctx.tlp_upper, input);
  assert(mpz_sizeinbase(input, 2) > 64);
  assert(siqs_tlp_complete(&ctx, input, input, labels, &count));
  assert(count == 3 && memcmp(labels, expected, sizeof(expected)) == 0);
  release_context(&ctx, &result);
  mpz_clears(n, input, prime, NULL);
}

#ifdef PSIQS
static void worker_handoff(void) {
  siqs_ctx_t master;
  siqs_factor_array_t result;
  psiqs_worker_t worker;
  siqs_ctx_t *ctx = &worker.ctx;
  mpz_t n, rest, y;
  mpz_init_set_ui(n, 10403); mpz_init_set_ui(rest, 13 * 17 * 19); mpz_init_set_ui(y, 991);
  test_context(&master, &result, n);
  memset(&worker, 0, sizeof(worker));
  ctx->original_n = n; ctx->params = master.params;
  ctx->largest_fb_prime = master.largest_fb_prime;
  ctx->tlp_policy = master.tlp_policy;
  ctx->buffer_relations = 1; ctx->shared_fb_tables = 1; ctx->fb = master.fb;
  mpz_init_set(ctx->n, n); mpz_init_set(ctx->kn, n);
  mpz_init_set(ctx->tlp_upper, master.tlp_upper);
  mpz_inits(ctx->eval.y, ctx->eval.q, ctx->eval.rest, NULL);
  siqs_factor_array_init(&worker.result, n); ctx->result = &worker.result;
  siqs_resolve_cofactor_3lp(ctx, rest, y, NULL, 0);
  assert(ctx->raw_count == 1 && ctx->raw[0]->lp3 == 0 && ctx->tlp == NULL);
  psiqs_merge_worker(&master, &worker);
  assert(ctx->raw_count == 0 && master.tlp->count == 1 && master.tlp_candidates == 1);
  siqs_tlp_flush(&master);
  assert(master.full_count == 1 && master.accepted_three_lp == 1);
  siqs_verify_full_relation(&master, master.full[0]);
  release_context(ctx, &worker.result);
  release_context(&master, &result);
  mpz_clears(n, rest, y, NULL);
}

/* Publishing a buffer must preserve the complete polynomial/candidate stream.
 * Compare the resumed worker with direct, uninterrupted Gray-code traversal;
 * the latter does not use the publication or continuation helpers. */
static void worker_family_continuation(void) {
  const struct {
    uint32_t budget, publication;
    int stop_first;
  } cases[] = {
    {1, 0, 0}, {4, 0, 0}, {8, 0, 0}, {16, 0, 0},
    {32, 0, 0}, {64, 0, 0}, {128, 0, 0},
    {128, 32, 0}, {128, 64, 0}, {128, 0, 1}
  };
  siqs_ctx_t master;
  siqs_factor_array_t result;
  siqs_poly_t dispatch;
  psiqs_pool_t pool;
  mpz_t n;
  uint32_t test;
  uint64_t records = 0;
  mpz_init_set_str(n, "1118962191081472546868486157778102360603851865973", 10);
  siqs_factor_array_init(&result, n);
  siqs_ctx_init(&master, n, n, &result, NULL, 0);
  master.params.max_large_primes = 3;
  master.params.lp_multiplier = 64.0;
  assert(siqs_ctx_allocate(&master));
#if SIQS_RESIDUAL_FIRST
  assert(master.residual_first);
  assert(master.candidate_at == NULL && master.resieve_model == NULL);
#else
  assert(master.candidate_at != NULL && master.resieve_model != NULL);
#endif
  siqs_poly_init(&master, &dispatch);
  assert(dispatch.b_limit == 128);
  memset(&pool, 0, sizeof(pool));
  assert(pthread_mutex_init(&pool.mutex, NULL) == 0);
  for (test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test) {
    psiqs_worker_t worker, reference;
    uint32_t i, publication = cases[test].publication;
    uint64_t chooser_state;
    psiqs_worker_init(&worker, &master, 0);
#if SIQS_RESIDUAL_FIRST
    assert(worker.ctx.residual_first);
    assert(worker.ctx.candidate_at == NULL && worker.ctx.resieve_model == NULL);
#endif
    psiqs_worker_init(&reference, &master, 0);
    worker.pool = &pool;
    assert(pthread_cond_init(&worker.work, NULL) == 0);
    assert(psiqs_pool_assign(&pool, &master, &dispatch, &worker, cases[test].budget));
    /* Request publication after work was assigned. The worker must observe it
     * at the shared checkpoint, without the coordinator rewriting its limit. */
    psiqs_pool_set_publication_interval(&pool, publication);
    pthread_mutex_lock(&pool.mutex);
    pool.stop = cases[test].stop_first;
    pthread_mutex_unlock(&pool.mutex);
    chooser_state = master.poly_rng.state;
    memcpy(reference.poly.a_index, worker.poly.a_index,
           worker.poly.q_count * sizeof(*worker.poly.a_index));
    for (i = 0; i < reference.poly.q_count; ++i)
      reference.ctx.fb_in_a[reference.poly.a_index[i]] = 1;
    mpz_set(reference.poly.A, worker.poly.A);
    mpz_set(reference.poly.DA, worker.poly.DA);
    siqs_first_B_and_roots(&reference.ctx, &reference.poly);
    siqs_set_family_sieve_initial(&reference.ctx);
    while (worker.family_polynomials < worker.poly.b_limit) {
      psiqs_sieve_family(&worker);
      assert(worker.polynomials != 0 && worker.polynomials <= worker.limit);
      assert(!worker.ctx.factor_found);
      if (publication != 0 || pool.stop) {
        uint32_t expected = pool.stop ? 32 : (publication + 31U) & ~31U;
        if (expected > worker.limit) expected = worker.limit;
        assert(worker.polynomials <= expected);
        if (worker.ctx.raw_count < SIQS_TLP_WORKER_RELATION_TARGET)
          assert(worker.polynomials == expected);
      }
      pthread_mutex_lock(&pool.mutex);
      pool.stop = 0;
      pthread_mutex_unlock(&pool.mutex);
      for (i = 0; i < worker.polynomials; ++i) {
        if (reference.family_polynomials != 0)
          assert(siqs_next_B(&reference.ctx, &reference.poly));
        siqs_sieve_polynomial(&reference.ctx, &reference.poly);
        ++reference.family_polynomials;
      }
      assert(worker.family_polynomials == reference.family_polynomials);
      assert(worker.poly.b_index + 1 == worker.family_polynomials);
      assert(worker.poly.b_index == reference.poly.b_index);
      assert(mpz_cmp(worker.poly.A, reference.poly.A) == 0);
      assert(mpz_cmp(worker.poly.B, reference.poly.B) == 0);
      assert(mpz_cmp(worker.poly.C, reference.poly.C) == 0);
      assert(memcmp(worker.ctx.root1, reference.ctx.root1,
                    master.params.fb_size * sizeof(uint32_t)) == 0);
      assert(memcmp(worker.ctx.root2, reference.ctx.root2,
                    master.params.fb_size * sizeof(uint32_t)) == 0);
      assert(worker.ctx.total_candidates == reference.ctx.total_candidates);
      assert(worker.ctx.tlp_candidates == reference.ctx.tlp_candidates);
      assert(worker.ctx.raw_count == reference.ctx.raw_count);
      for (i = 0; i < worker.ctx.raw_count; ++i) {
        const siqs_raw_relation_t *a = worker.ctx.raw[i], *b = reference.ctx.raw[i];
        uint32_t j;
        assert(mpz_cmp(a->y, b->y) == 0 && a->nfactors == b->nfactors);
        assert(a->lp1 == b->lp1 && a->lp2 == b->lp2 && a->lp3 == b->lp3);
        for (j = 0; j < a->nfactors; ++j) {
          assert(siqs_raw_factor_row(a, j) == siqs_raw_factor_row(b, j));
          assert(siqs_raw_factor_exponent(a, j) == siqs_raw_factor_exponent(b, j));
        }
      }
      records += worker.ctx.raw_count;
      while (worker.ctx.raw_count != 0)
        siqs_raw_relation_free(&worker.ctx, worker.ctx.raw[--worker.ctx.raw_count]);
      while (reference.ctx.raw_count != 0)
        siqs_raw_relation_free(&reference.ctx, reference.ctx.raw[--reference.ctx.raw_count]);
      psiqs_raw_arena_reset(&worker.ctx.raw_arena);
      psiqs_raw_arena_reset(&reference.ctx.raw_arena);
      if (worker.family_polynomials != worker.poly.b_limit) {
        assert(psiqs_pool_assign(&pool, &master, &dispatch, &worker, cases[test].budget));
        if (publication == 64) {
          publication = 32;
          psiqs_pool_set_publication_interval(&pool, publication);
        }
        assert(master.poly_rng.state == chooser_state);
      }
    }
    assert(reference.family_polynomials == reference.poly.b_limit);
    assert(!siqs_next_B(&reference.ctx, &reference.poly));
    assert(pthread_cond_destroy(&worker.work) == 0);
    psiqs_worker_clear(&worker);
    psiqs_worker_clear(&reference);
  }
  assert(records != 0);
  assert(pthread_mutex_destroy(&pool.mutex) == 0);
  siqs_poly_clear(&master, &dispatch);
  release_context(&master, &result);
  mpz_clear(n);
}

#endif

static uint64_t little64(const unsigned char *p) {
  unsigned i; uint64_t value = 0;
  for (i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
  return value;
}

static void completion_corpus(const char *path) {
  unsigned char row[64];
  FILE *file = fopen(path, "rb");
  siqs_ctx_t ctx; siqs_factor_array_t result;
  mpz_t n, input, gcd;
  uint64_t checked = 0;
  assert(file != NULL);
  mpz_init_set_ui(n, 10403); mpz_inits(input, gcd, NULL);
  test_context(&ctx, &result, n);
  ctx.largest_fb_prime = 11946059; ctx.params.large_prime_bound = 2090560325;
  while (fread(row, 1, 64, file) == 64) {
    uint64_t words[2], labels[3]; uint32_t count, j;
    words[0] = little64(row + 8); words[1] = little64(row + 16);
    mpz_import(input, 2, -1, sizeof(uint64_t), 0, 0, words);
    words[0] = little64(row + 24); words[1] = little64(row + 32);
    mpz_import(gcd, 2, -1, sizeof(uint64_t), 0, 0, words);
    assert(siqs_tlp_complete(&ctx, input, gcd, labels, &count));
    assert(count == 3);
    for (j = 0; j < 3; ++j) assert(labels[j] == little64(row + 40 + 8 * j));
    ++checked;
  }
  assert(feof(file) && checked == 12184); fclose(file);
  release_context(&ctx, &result); mpz_clears(n, input, gcd, NULL);
  printf("{\"verified_usable_completions\":%"PRIu64",\"all_labels_match\":true}\n", checked);
}

#include "check-tlp-gcd-job-inc.c"
#include "check-tlp-worker-job-inc.c"

#include "check-tlp-policy-inc.c"

int main(int argc, char **argv) {
  if (argc == 2) { completion_corpus(argv[1]); return 0; }
#ifndef SIQS_NO_JOURNAL
  journal_checkpoint_policy();
  journal_pending_lp();
#endif
  policy_table_kernels();
  triples_and_dependency();
  solve_finds_factor_during_drain();
  rejects_and_repeated_labels();
  tinyecm_completion();
  wide_siqs_completion();
#ifdef PSIQS
  worker_handoff();
  worker_family_continuation();
#endif
  tlp_gcd_job_kernels();
  tlp_worker_job_kernels();
  tlp_policy_kernels();
#ifdef PSIQS
  puts("{\"gcd_job_visibility\":true,\"gcd_job_errors_and_cleanup\":true,\"worker_job_ownership\":true,\"worker_job_stale_ready\":true,\"worker_job_stop\":true,\"worker_job_budget\":true}");
#endif
  puts("{\"three_label_dependency\":true,\"factor_extraction\":true,\"presolve_drain_factor\":true,\"GCD_one_no_primality\":true,\"repeated_labels\":true,\"partial_batch_drain\":true,\"wide_siqs_completion\":true}");
#ifdef PSIQS
  puts("{\"worker_handoff\":true,\"worker_family_continuation\":true,\"worker_publication_checkpoint\":true}");
#endif
  return 0;
}
