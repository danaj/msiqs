/* Worker task/lifetime checks. Included by check.c after the ordinary private
 * SIQS helpers. These tests use real pool threads and a separate GCD context. */

#ifdef PSIQS

static psiqs_pool_t *tlp_job_test_pool_count(siqs_ctx_t *master,
                                      siqs_tlp_gcd_job_t *stopped_job, unsigned count) {
  psiqs_pool_t *pool = siqs_calloc(1, sizeof(*pool));
  psiqs_worker_t *worker;
  siqs_ctx_t *ctx;
  unsigned i;
  assert(count>=1 && count<=PSIQS_MAX_THREADS && (stopped_job==NULL || count==1));
  pool->count = count;
  pool->workers = siqs_calloc(count, sizeof(*pool->workers));
  pool->ready = siqs_calloc(count, sizeof(*pool->ready));
  pool->master = master;
  assert(pthread_mutex_init(&pool->mutex, NULL) == 0);
  assert(pthread_cond_init(&pool->done, NULL) == 0);
  for(i=0;i<count;++i) {
    worker = pool->workers+i; worker->index=i;
    worker->pool = pool;
    assert(pthread_cond_init(&worker->work, NULL) == 0);
    ++pool->conditions;
    /* Deliberately allocate only raw/result ownership. A GCD task must never
     * enter the sieve or access uninitialized polynomial/root scratch. */
    ctx = &worker->ctx;
    ctx->original_n = master->original_n;
    ctx->params = master->params;
    ctx->tlp_policy = master->tlp_policy;
    ctx->buffer_relations = 1;
    ctx->shared_fb_tables = 1;
    ctx->fb = master->fb;
    ctx->largest_fb_prime = master->largest_fb_prime;
    mpz_init_set(ctx->n, master->n);
    mpz_init_set(ctx->kn, master->kn);
    mpz_init_set(ctx->tlp_upper, master->tlp_upper);
    mpz_inits(ctx->eval.y, ctx->eval.q, ctx->eval.rest, NULL);
    siqs_factor_array_init(&worker->result, ctx->n);
    ctx->result = &worker->result;
#ifdef SIQS_TIMING
    ctx->params.bits = SIQS_TIMING_MIN_BITS;
    pool->timing_enabled = 1;
#endif
    if (stopped_job != NULL) {
      /* Stop is visible before thread creation: this deterministically covers
       * an assigned GCD that has not begun when shutdown is requested. */
      pool->stop = 1;
      worker->task = PSIQS_TASK_GCD;
      worker->gcd_job = stopped_job;
      worker->state = PSIQS_WORK;
    }
    assert(pthread_create(&worker->thread, NULL, psiqs_pool_worker, worker) == 0);
    worker->started = 1;
    ++pool->live;
  }
  master->tlp_submit_gcd = psiqs_pool_submit_gcd;
  master->tlp_submit_data = pool;
  return pool;
}

static psiqs_pool_t *tlp_job_test_pool(siqs_ctx_t *master,
                                        siqs_tlp_gcd_job_t *stopped_job) {
  return tlp_job_test_pool_count(master,stopped_job,1);
}

static void tlp_job_test_pool_clear(psiqs_pool_t *pool) {
  unsigned i;
  psiqs_pool_join(pool);
  for(i=0;i<pool->count;++i)
    release_context(&pool->workers[i].ctx, &pool->workers[i].result);
  /* No sieve scratch was initialized; pool destruction must not clear this
   * deliberately partial worker context a second time. */
  psiqs_pool_destroy(pool);
}

static void tlp_worker_job_ownership(void) {
  siqs_ctx_t master;
  siqs_factor_array_t result;
  siqs_tlp_prime_store *store = NULL;
  siqs_tlp_batch *batch = NULL;
  siqs_tlp_gcd_job_t job;
  siqs_tlp_gcd_job_result_t observation;
  psiqs_pool_t *pool;
  psiqs_worker_t *worker;
  siqs_raw_relation_t *pending, *smooth;
  siqs_raw_block_t *arena;
  mpz_t n, rest, y, inputs[2], gcds[2];
  unsigned i;
  mpz_init_set_ui(n, 10403);
  mpz_init_set_ui(rest, 13 * 17 * 19);
  mpz_init_set_ui(y, 991);
  for (i = 0; i < 2; ++i) { mpz_init(inputs[i]); mpz_init(gcds[i]); }
  mpz_set_ui(inputs[0], 13 * 17 * 19);
  mpz_set_ui(inputs[1], 13 * 29);
  assert(siqs_tlp_prime_store_create(&store, 11, 23) == SIQS_TLP_OK);
  assert(siqs_tlp_batch_create(&batch, store, 32) == SIQS_TLP_OK);
  assert(siqs_tlp_gcd_job_init(&job, batch, 1));
  test_context(&master, &result, n);
  pool = tlp_job_test_pool(&master, NULL);
  worker = pool->workers;
  pending = siqs_tlp_pending_new(&worker->ctx, y, NULL, 0, rest);
  siqs_store_raw(&worker->ctx, pending);
  mpz_set_ui(y, 1);
  smooth = siqs_raw_relation_new(&worker->ctx, y, NULL, 0, 1, 1);
  siqs_store_raw(&worker->ctx, smooth);
  arena = worker->ctx.raw_arena.current;
  worker->ctx.total_candidates = 123;
  worker->ctx.tlp_candidates = 7;
  worker->ctx.cofactor_rng.state = UINT64_C(0x123456789abcdef);
  worker->family_assigned = 1;
  worker->family_polynomials = 19;
  worker->poly.b_index = 18;
  worker->poly.b_limit = 128;
  worker->limit = 41;
  worker->polynomials = 23;
  worker->gcd_reserved = 18;
#ifdef SIQS_TIMING
  worker->result.profile.worker_cpu_us = 555;
  worker->result.profile.worker_active_us = 666;
  worker->result.profile.poly_resieve_us = 777;
  worker->result.profile.worker_polynomials = 37;
  worker->result.profile.publications = 9;
#endif
  assert(siqs_tlp_gcd_job_prepare(&job, inputs, gcds, 2));
  /* Only a currently merging idle sieve worker may be borrowed. Rejected
   * submissions must leave the caller free to run its prepared job itself. */
  assert(!psiqs_pool_submit_gcd(pool, &job));
  pool->merging_worker = worker;
  master.factor_found = 1;
  assert(!psiqs_pool_submit_gcd(pool, &job));
  master.factor_found = 0;
  worker->ctx.factor_found = 1;
  assert(!psiqs_pool_submit_gcd(pool, &job));
  worker->ctx.factor_found = 0;
  master.params.max_large_primes = 2;
  assert(!psiqs_pool_submit_gcd(pool, &job));
  master.params.max_large_primes = 3;
  assert(psiqs_pool_submit_gcd(pool, &job));
  assert(!psiqs_pool_submit_gcd(pool, &job));
  pool->merging_worker = NULL;
  assert(siqs_tlp_gcd_job_wait(&job, &observation));
  assert(observation.status == SIQS_TLP_OK);
  assert(mpz_cmp_ui(gcds[0], 13 * 17 * 19) == 0);
  assert(mpz_cmp_ui(gcds[1], 13) == 0);
  /* Reuse the job before dequeueing its old worker completion. That READY
   * event must release the worker without waiting on/consuming this new job. */
  mpz_set_ui(inputs[0], 1009);
  assert(siqs_tlp_gcd_job_prepare(&job, inputs, gcds, 1));
  assert(psiqs_pool_take(pool) == worker);
  assert(worker->task == PSIQS_TASK_GCD);
  assert(worker->limit == 0 && worker->polynomials == 0);
  assert(worker->family_polynomials == 19 && worker->poly.b_index == 18);
  assert(worker->poly.b_limit == 128 && worker->family_assigned);
  assert(worker->ctx.raw_count == 2 && worker->ctx.raw[0] == pending &&
         worker->ctx.raw[1] == smooth && worker->ctx.raw_arena.current == arena);
  assert(mpz_cmp_ui(pending->y, 991) == 0 && pending->lp3 == 0);
  assert(mpz_cmp_ui(smooth->y, 1) == 0 && smooth->lp2 == 1);
  assert(worker->ctx.total_candidates == 123 && worker->ctx.tlp_candidates == 7);
  assert(worker->ctx.cofactor_rng.state == UINT64_C(0x123456789abcdef));
  assert(worker->result.count == 1 && mpz_cmp(worker->result.values[0], n) == 0);
#ifdef SIQS_TIMING
  assert(worker->result.profile.worker_cpu_us == 555);
  assert(worker->result.profile.worker_active_us == 666);
  assert(worker->result.profile.poly_resieve_us == 777);
  assert(worker->result.profile.worker_polynomials == 37);
  assert(worker->result.profile.publications == 9);
#endif
  assert(psiqs_release_gcd_worker(&master, worker) == 18);
  assert(worker->task == PSIQS_TASK_SIEVE && worker->gcd_job == NULL);
  assert(worker->gcd_reserved == 0);
  assert(!siqs_tlp_gcd_job_ready(&job));
  assert(!siqs_tlp_gcd_job_clear(&job)); /* Still prepared and unconsumed. */
  siqs_tlp_gcd_job_run(&job);
  assert(siqs_tlp_gcd_job_wait(&job, &observation));
  assert(observation.status == SIQS_TLP_OK && mpz_cmp_ui(gcds[0], 1) == 0);
  assert(siqs_tlp_gcd_job_clear(&job));
  tlp_job_test_pool_clear(pool);
  assert(master.tlp_submit_gcd == NULL && master.tlp_submit_data == NULL);
  release_context(&master, &result);
  siqs_tlp_batch_destroy(batch);
  siqs_tlp_prime_store_destroy(store);
  for (i = 0; i < 2; ++i) { mpz_clear(inputs[i]); mpz_clear(gcds[i]); }
  mpz_clears(n, rest, y, NULL);
}

static void tlp_worker_job_stop(void) {
  siqs_ctx_t master;
  siqs_factor_array_t result;
  siqs_tlp_prime_store *store = NULL;
  siqs_tlp_batch *batch = NULL;
  siqs_tlp_gcd_job_t job;
  siqs_tlp_gcd_job_result_t observation;
  psiqs_pool_t *pool;
  psiqs_worker_t *worker;
  mpz_t n, input[1], gcd[1];
  mpz_init_set_ui(n, 10403);
  mpz_init_set_ui(input[0], 13 * 29);
  mpz_init(gcd[0]);
  test_context(&master, &result, n);
  assert(siqs_tlp_prime_store_create(&store, 11, 23) == SIQS_TLP_OK);
  assert(siqs_tlp_batch_create(&batch, store, 32) == SIQS_TLP_OK);
  assert(siqs_tlp_gcd_job_init(&job, batch, 1));
  assert(siqs_tlp_gcd_job_prepare(&job, input, gcd, 1));
  pool = tlp_job_test_pool(&master, &job);
  worker = pool->workers;
  psiqs_pool_join(pool);
  assert(pool->live == 0 && pool->queued == 1 && !worker->started);
  assert(worker->state == PSIQS_READY && worker->task == PSIQS_TASK_GCD);
  assert(worker->polynomials == 0 && worker->limit == 0);
  assert(worker->family_polynomials == 0 && worker->ctx.raw_count == 0);
  assert(siqs_tlp_gcd_job_ready(&job));
  assert(siqs_tlp_gcd_job_wait(&job, &observation));
  assert(observation.status == SIQS_TLP_OK && mpz_cmp_ui(gcd[0], 13) == 0);
#ifdef SIQS_TIMING
  assert(worker->result.profile.worker_cpu_us == 0);
  assert(worker->result.profile.worker_active_us == 0);
  assert(worker->result.profile.worker_polynomials == 0);
  assert(worker->result.profile.publications == 0);
#endif
  assert(psiqs_pool_take(pool) == worker);
  assert(psiqs_release_gcd_worker(&master, worker) == 0);
  assert(siqs_tlp_gcd_job_clear(&job));
  tlp_job_test_pool_clear(pool);
  assert(master.tlp_submit_gcd == NULL && master.tlp_submit_data == NULL);
  release_context(&master, &result);
  siqs_tlp_batch_destroy(batch);
  siqs_tlp_prime_store_destroy(store);
  mpz_clears(n, input[0], gcd[0], NULL);
}

/* Enter the production collector near its normal budget boundary instead of
 * introducing a test-only runtime knob. GCD jobs must not consume/refund
 * polynomial reservations, including final budgets not divisible by 32. */
static void tlp_worker_job_budgets(void) {
  static const uint32_t budgets[] = {1, 31, 32, 33, 67, 2050};
  mpz_t n;
  unsigned test;
  mpz_init_set_str(n, "1118962191081472546868486157778102360603851865973", 10);
  for (test = 0; test < sizeof(budgets) / sizeof(budgets[0]); ++test) {
    siqs_ctx_t master;
    siqs_factor_array_t result;
    siqs_poly_t dispatch;
    uint64_t maximum, polynomials, families = 0;
    uint32_t budget = budgets[test];
    uint32_t next_matrix_check = UINT32_MAX;
    siqs_factor_array_init(&result, n);
    siqs_ctx_init(&master, n, n, &result, NULL, 0);
    master.params.max_large_primes = 3;
    master.params.lp_multiplier = 64.0;
    master.nthreads = 2;
    assert(siqs_ctx_allocate(&master));
    siqs_poly_init(&master, &dispatch);
    maximum = siqs_polynomial_limit(&master.params);
    assert(maximum != 0);
    if (maximum < budget) budget = (uint32_t)maximum;
    polynomials = maximum - budget;
    (void)psiqs_collect_relations(&master, &dispatch, UINT32_MAX,
                                &next_matrix_check, &families, &polynomials);
    assert(polynomials == maximum && families != 0 && !master.factor_found);
    /* Short publications can spread a final budget across workers instead
     * of reserving a whole family for one worker. Every family has at most
     * b_limit polynomials; only the live workers can finish with a prefix. */
    assert(families >= ((uint64_t)budget + dispatch.b_limit - 1U) / dispatch.b_limit);
    assert(families <= ((uint64_t)budget + dispatch.b_limit - 1U) / dispatch.b_limit + master.nthreads - 1U);
    assert(families <= budget);
    if (budget <= SIQS_TLP_WORKER_PUBLISH_POLYNOMIALS) assert(families == 1);
    assert(master.tlp_submit_gcd == NULL && master.tlp_submit_data == NULL);
    assert(siqs_tlp_pending_count(&master) == 0);
    assert(master.tlp_candidates == master.tlp_gcd_one + master.tlp_gcd_proper +
                                    master.tlp_gcd_whole);
#ifdef SIQS_TIMING
    if (master.params.bits >= SIQS_TIMING_MIN_BITS)
      assert(result.profile.worker_polynomials == budget);
#endif
    siqs_poly_clear(&master, &dispatch);
    release_context(&master, &result);
  }
  mpz_clear(n);
}

static void tlp_worker_job_kernels(void) {
  tlp_worker_job_ownership();
  tlp_worker_job_stop();
  tlp_worker_job_budgets();
}

#else

static void tlp_worker_job_kernels(void) { }

#endif
