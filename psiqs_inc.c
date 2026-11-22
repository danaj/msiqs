/*============================================================================
  Parallel family collection for SIQS.

  Included only by siqs.c when PSIQS is defined.  Do not compile this file
  separately: it uses the shared private SIQS types and static helpers.

  Workers share immutable factor-base tables, own polynomial/sieve scratch,
  and buffer raw relations.  The caller assigns distinct A families and merges
  each completed buffer into one relation graph; factor partition updates and
  relation merging remain serial.

  Copyright (c) 2026 Dana Jacobsen
============================================================================*/

#include <pthread.h>

typedef struct psiqs_pool_t psiqs_pool_t;

/* Bound delivery latency without discarding the rest of an A family.
 * Zero preserves whole-family publication. */
#ifndef PSIQS_PUBLICATION_POLYNOMIALS
# define PSIQS_PUBLICATION_POLYNOMIALS 0U
#endif


typedef enum { PSIQS_IDLE, PSIQS_WORK, PSIQS_READY } psiqs_worker_state_t;
typedef enum { PSIQS_TASK_SIEVE, PSIQS_TASK_GCD } psiqs_worker_task_t;

typedef struct {
  siqs_ctx_t ctx;
  siqs_poly_t poly;
  siqs_factor_array_t result;
  uint32_t limit;        /* Reserved polynomial budget for this publication. */
  uint32_t polynomials;  /* Work since the previous publication. */
  uint32_t family_polynomials;
  int family_assigned;
  psiqs_pool_t *pool;
  pthread_t thread;
  pthread_cond_t work;
  uint32_t index;
  psiqs_worker_state_t state;
  psiqs_worker_task_t task;
  siqs_tlp_gcd_job_t *gcd_job;
  uint32_t gcd_reserved; /* Coordinator-owned sieve tail held during a GCD. */
  int started;
  int scratch_initialized; /* Independent of thread lifetime and condition. */
#ifdef SIQS_TIMING
  uint64_t published_at;
#endif
} psiqs_worker_t;

struct psiqs_pool_t {
  psiqs_worker_t *workers;
  uint32_t *ready;
  uint32_t count, initialized, conditions, live;
  uint32_t head, tail, queued;
  pthread_mutex_t mutex;
  pthread_cond_t done;
  uint32_t publication_polynomials; /* Coordinator-owned; workers read under mutex. */
  int stop;
  siqs_ctx_t *master;
  psiqs_worker_t *merging_worker; /* Coordinator-only dispatch opportunity. */
  int merging_dispatched;
#ifdef SIQS_TIMING
  int timing_enabled;
#endif
};

/* The coordinator can shorten a running publication without changing its
 * reserved work or polynomial state. Workers observe the request at the
 * existing stop checkpoint; zero allows a whole family, subject to buffers. */
static void psiqs_pool_set_publication_interval(psiqs_pool_t *pool,
                                                uint32_t polynomials) {
  pthread_mutex_lock(&pool->mutex);
  pool->publication_polynomials = polynomials;
  pthread_mutex_unlock(&pool->mutex);
}

/* Reuse the parked worker whose raw buffer the coordinator is consuming.
 * A GCD task never touches that buffer, sieve scratch or factor partition.
 * The completed sieve reservation has already been refunded/accounted; this
 * task reserves no polynomial work and may be dispatched only once per merge. */
static int psiqs_pool_submit_gcd(void *data, struct siqs_tlp_gcd_job *job) {
  psiqs_pool_t *pool = (psiqs_pool_t *)data;
  psiqs_worker_t *worker;
  int dispatched = 0;
  if (pool == NULL || job == NULL || pool->master == NULL ||
      pool->master->params.max_large_primes != 3 || pool->master->factor_found ||
      pool->merging_dispatched || (worker = pool->merging_worker) == NULL)
    return 0;
  pthread_mutex_lock(&pool->mutex);
  if (!pool->stop && worker->started && worker->state == PSIQS_IDLE &&
      worker->task == PSIQS_TASK_SIEVE && !worker->ctx.factor_found) {
    worker->task = PSIQS_TASK_GCD;
    worker->gcd_job = job;
    worker->limit = worker->polynomials = 0;
    worker->state = PSIQS_WORK;
    pool->merging_dispatched = 1;
    pthread_cond_signal(&worker->work);
    dispatched = 1;
  }
  pthread_mutex_unlock(&pool->mutex);
  return dispatched;
}

static void psiqs_pool_unbind_gcd(psiqs_pool_t *pool) {
  if (pool->master != NULL && pool->master->tlp_submit_data == pool) {
    pool->master->tlp_submit_gcd = NULL;
    pool->master->tlp_submit_data = NULL;
  }
  pool->merging_worker = NULL;
}

/* Workers own every writable field and borrow the collector's immutable FB
 * tables.  Reuse scratch cleanup without allocating cycle/dependency scratch
 * or a relation graph, and without rebuilding the common factor base. */
static void psiqs_worker_init(psiqs_worker_t *worker,
                              const siqs_ctx_t *master, uint32_t index) {
  siqs_ctx_t *ctx = &worker->ctx;
  memset(worker, 0, sizeof(*worker));
  ctx->original_n = master->original_n;
  ctx->params = master->params;
  ctx->tlp_policy = master->tlp_policy;
#if SIQS_RESIDUAL_FIRST
  ctx->residual_first = master->residual_first;
#endif
  ctx->multiplier = master->multiplier;
  ctx->largest_fb_prime = master->largest_fb_prime;
  ctx->buffer_relations = 1;
  ctx->verbose = master->verbose;
  mpz_init_set(ctx->n, master->n);
  mpz_init_set(ctx->kn, master->kn);
  mpz_init(ctx->eval.y);
  mpz_init(ctx->eval.q);
  mpz_init(ctx->eval.rest);
  mpz_init_set(ctx->tlp_upper, master->tlp_upper);
  ctx->shared_fb_tables = 1;
  ctx->fb = master->fb;
  ctx->prime = master->prime;
  ctx->fb_reciprocal = master->fb_reciprocal;
  ctx->sieve_logp = master->sieve_logp;
  ctx->resieve_model = master->resieve_model;
  ctx->cofactor_rng.state = siqs_mix64(
      master->cofactor_rng.state ^
      ((uint64_t)index + 1U) * UINT64_C(0x9e3779b97f4a7c15));
  siqs_factor_array_init(&worker->result, ctx->n);
  ctx->result = &worker->result;
  siqs_workspace_allocate(ctx);
  siqs_poly_init(ctx, &worker->poly);
  worker->scratch_initialized = 1;
}

/* Reserve work without changing the current polynomial. Checkpoint requests
 * and buffer bounds can publish early; the coordinator refunds unused work.
 * Remaining polynomials stay with this worker and the same A family. */
static void psiqs_prepare_publication(psiqs_worker_t *worker, uint32_t limit) {
  uint32_t remaining = worker->poly.b_limit - worker->family_polynomials;
  /* Standalone polynomial preparation, including benchmarks, has no pool. */
  uint32_t publication = worker->pool != NULL
                       ? worker->pool->publication_polynomials : 0;
  if (limit > remaining) limit = remaining;
  if (publication != 0 && limit > publication) limit = publication;
  worker->limit = limit;
  worker->polynomials = 0;
}

/* Select distinct A values serially, then let each worker initialize roots. */
static int psiqs_assign_family(siqs_ctx_t *master, siqs_poly_t *dispatch,
                               psiqs_worker_t *worker, uint32_t limit) {
  siqs_ctx_t *ctx = &worker->ctx;
  siqs_poly_t *poly = &worker->poly;
  uint32_t i;
  if (!siqs_choose_A(master, dispatch))
    return 0;
  if (master->journal != NULL)
    siqs_journal_family(master, dispatch);
  for (i = 0; i < poly->q_count; i++)
    if (poly->a_index[i] < ctx->params.fb_size)
      ctx->fb_in_a[poly->a_index[i]] = 0;
  memcpy(poly->a_index, dispatch->a_index,
         (size_t)poly->q_count * sizeof(*poly->a_index));
  for (i = 0; i < poly->q_count; i++)
    ctx->fb_in_a[poly->a_index[i]] = 1;
  mpz_set(poly->A, dispatch->A);
  mpz_set(poly->DA, dispatch->DA);
  worker->family_polynomials = 0;
  worker->family_assigned = 1;
  psiqs_prepare_publication(worker, limit);
  return 1;
}

/* Workers buffer raw relations without changing the caller's relation graph.
 * Cofactor resolution may run an independent quiet serial SIQS solver and
 * report a rare split miss using this call's verbosity. */
static void *psiqs_sieve_family(void *argument) {
  psiqs_worker_t *worker = (psiqs_worker_t *)argument;
  siqs_ctx_t *ctx = &worker->ctx;
  siqs_poly_t *poly = &worker->poly;
  if (worker->family_polynomials == 0) {
    siqs_first_B_and_roots(ctx, poly);
    siqs_set_family_sieve_initial(ctx);
  } else {
    /* Publication leaves roots and Gray-code state at the last polynomial.
     * Advance once before continuing the same family. */
    if (!siqs_next_B(ctx, poly))
      croak("PSIQS: attempted to continue an exhausted family");
  }
  do {
    siqs_sieve_polynomial(ctx, poly);
    worker->polynomials++;
    worker->family_polynomials++;
    if (ctx->factor_found || worker->polynomials >= worker->limit ||
        (ctx->params.max_large_primes == 3 &&
         ctx->raw_count >= SIQS_TLP_WORKER_RELATION_TARGET))
      break;
    /* A q=12 family has 2048 polynomials.  At readiness, finish only
     * a short prefix instead of delaying join for the rest of that family.
     * Keep every buffered relation; retries select fresh A values.  Read the
     * existing stop flag under its mutex, outside the sieve/root kernels. */
    if ((worker->polynomials & 31U) == 0) {
      int stop;
      pthread_mutex_lock(&worker->pool->mutex);
      stop = worker->pool->stop ||
             (worker->pool->publication_polynomials != 0 &&
              worker->polynomials >= worker->pool->publication_polynomials);
      pthread_mutex_unlock(&worker->pool->mutex);
      if (stop)
        break;
    }
  } while (siqs_next_B(ctx, poly));
  return NULL;
}

/* After every buffered GMP value is cleared, retain one normal-sized block
 * for the next family.  Older blocks and oversized one-offs are released;
 * final worker cleanup still frees the retained block. */
static void psiqs_raw_arena_reset(siqs_raw_arena_t *arena) {
  siqs_raw_block_t *block = arena->current;
  if (block != NULL &&
      block->capacity <= SIQS_RAW_BLOCK_MAX - sizeof(*block)) {
    arena->current = block->previous;
    siqs_raw_arena_clear(arena);
    block->previous = NULL;
    block->used = 0;
    arena->current = block;
  } else {
    siqs_raw_arena_clear(arena);
  }
}

#ifdef SIQS_TIMING
/* A parked worker owns no live writes. Also call after join for terminal
 * parked time that was accumulated after its last ordinary handoff. */
static void psiqs_merge_profile(siqs_ctx_t *master, psiqs_worker_t *worker) {
#define SIQS_PROFILE_MERGE(name) \
  master->result->profile.name += worker->result.profile.name; \
  worker->result.profile.name = 0;
  SIQS_PROFILE_FIELDS(SIQS_PROFILE_MERGE)
#undef SIQS_PROFILE_MERGE
}
#endif

/* A READY event returns only its executor. The job may already have been
 * reaped and reused on another worker, so never inspect it through this slot. */
static uint32_t psiqs_release_gcd_worker(siqs_ctx_t *master, psiqs_worker_t *worker) {
  uint32_t reserved = worker->gcd_reserved;
  worker->gcd_reserved = 0;
  worker->task = PSIQS_TASK_SIEVE;
  worker->gcd_job = NULL;
#ifdef SIQS_TIMING
  psiqs_merge_profile(master, worker);
#endif
  siqs_tlp_poll(master);
  return reserved;
}

/* Copy buffered records into the collector's arena; never transfer pointers
 * between arenas.  All partials enter the same graph, including cross-worker
 * cycles.  Clear worker GMP values before resetting its buffer storage. */
static void psiqs_merge_worker(siqs_ctx_t *master, psiqs_worker_t *worker) {
  siqs_ctx_t *ctx = &worker->ctx;
  uint32_t i;
#ifdef SIQS_TIMING
  uint64_t timing_start = master->params.bits >= SIQS_TIMING_MIN_BITS ? siqs_timing_now() : 0;
#endif
  master->total_candidates += ctx->total_candidates;
  master->split_attempts += ctx->split_attempts;
  master->split_square += ctx->split_square;
  master->split_siqs += ctx->split_siqs;
  master->split_squfof += ctx->split_squfof;
  master->split_ecm += ctx->split_ecm;
  master->split_fail += ctx->split_fail;
  master->split_rejected += ctx->split_rejected;
  master->tlp_candidates += ctx->tlp_candidates; ctx->tlp_candidates = 0;
  if (ctx->params.max_large_primes == 3) {
    master->tlp_r2_attempts += ctx->tlp_r2_attempts; ctx->tlp_r2_attempts = 0;
    master->tlp_r2_resolved += ctx->tlp_r2_resolved; ctx->tlp_r2_resolved = 0;
    master->tlp_r2_rejected += ctx->tlp_r2_rejected; ctx->tlp_r2_rejected = 0;
    master->tlp_r2_fallback += ctx->tlp_r2_fallback; ctx->tlp_r2_fallback = 0;
    master->tlp_gap_rejected += ctx->tlp_gap_rejected; ctx->tlp_gap_rejected = 0;
  }
#ifdef SIQS_TIMING
  master->result->cofactor_time += worker->result.cofactor_time;
  master->result->cofactor_calls += worker->result.cofactor_calls;
  worker->result.cofactor_time = worker->result.cofactor_calls = 0;
  master->result->primality_time += worker->result.primality_time;
  master->result->primality_prime += worker->result.primality_prime;
  master->result->primality_composite += worker->result.primality_composite;
  worker->result.primality_time = worker->result.primality_prime =
      worker->result.primality_composite = 0;
  psiqs_merge_profile(master, worker);
#endif
  ctx->total_candidates = 0;
  ctx->split_attempts = 0;
  ctx->split_square = 0;
  ctx->split_siqs = 0;
  ctx->split_squfof = 0;
  ctx->split_ecm = 0;
  ctx->split_fail = 0;
  ctx->split_rejected = 0;
  /* A polynomial zero can discover a divisor without emitting a relation.
   * Refine the parent's partition only here, while this worker is parked. */
  if (ctx->factor_found) {
    for (i = 0; i < worker->result.count; i++)
      if (siqs_insert_divisor(master->result, worker->result.values[i]))
        master->factor_found = 1;
    siqs_bids_note_progress(master);
  }
  for (i = 0; i < ctx->raw_count; i++) {
    const siqs_raw_relation_t *raw = ctx->raw[i];
    if (!master->factor_found) {
      siqs_factor_t *factors = siqs_eval_reserve_factors(
          master, raw->nfactors);
      siqs_raw_relation_t *copy;
      uint32_t j;
      for (j = 0; j < raw->nfactors; j++) {
        factors[j].row = siqs_raw_factor_row(raw, j);
        factors[j].exponent = siqs_raw_factor_exponent(raw, j);
      }
#if SIQS_RESIDUAL_FIRST
      if (raw->rf_norm != NULL) {
        copy = siqs_rf_packet_new(&master->rf->pending_arena, raw->y,
            factors, raw->nfactors, raw->rf_norm->norm, raw->rf_norm->distinct);
        siqs_rf_queue(master, copy);
      } else
#endif
      if (raw->lp3 == 0) {
        uint64_t words[2] = {raw->lp1, raw->lp2};
        mpz_import(master->eval.rest, 2, -1, sizeof(uint64_t), 0, 0, words);
        copy = siqs_tlp_pending_new(master, raw->y, factors, raw->nfactors, master->eval.rest);
        siqs_tlp_queue(master, copy);
      } else {
        copy = siqs_raw_relation_new3(master, raw->y, factors,
            raw->nfactors, raw->lp1, raw->lp2, raw->lp3);
        siqs_accept_raw_relation(master, copy);
      }
    }
  }
#if SIQS_RESIDUAL_FIRST
  if (master->rf != NULL && !master->factor_found) siqs_rf_poll(master);
#endif
  while (ctx->raw_count != 0)
    siqs_raw_relation_free(ctx, ctx->raw[--ctx->raw_count]);
  psiqs_raw_arena_reset(&ctx->raw_arena);
  if (master->journal != NULL) {
    if (master->params.max_large_primes == 3) sj_maybe_flush(master);
    else sj_flush(master);
  }
#ifdef SIQS_TIMING
  if (timing_start) master->result->profile.merge_us += siqs_timing_elapsed(timing_start);
#endif
}

static void psiqs_worker_clear(psiqs_worker_t *worker) {
  if (!worker->scratch_initialized)
    return;
  siqs_poly_clear(&worker->ctx, &worker->poly);
  siqs_ctx_clear(&worker->ctx);
  free(worker->result.primality);
  gmp_siqs_free(worker->result.values, worker->result.count);
  memset(&worker->result, 0, sizeof(worker->result));
  worker->scratch_initialized = 0;
}

/* Each worker has one sieve-result buffer. A GCD task can use that same thread
 * while the caller consumes its buffer. It touches only the submitted job,
 * then publishes a distinct completion event. The queue still holds at most
 * one event per worker; every ordinary publication parks its producer. */
static void *psiqs_pool_worker(void *argument) {
  psiqs_worker_t *worker = (psiqs_worker_t *)argument;
  psiqs_pool_t *pool = worker->pool;
#ifdef SIQS_TIMING
  int timing = worker->ctx.params.bits >= SIQS_TIMING_MIN_BITS;
  uint64_t active_start = 0, cpu_start = 0;
#endif
  pthread_mutex_lock(&pool->mutex);
  for (;;) {
    int gcd_task;
    while (!pool->stop && worker->state != PSIQS_WORK)
      pthread_cond_wait(&worker->work, &pool->mutex);
    if (pool->stop
        && !(worker->state == PSIQS_WORK && worker->task == PSIQS_TASK_GCD)
       ) {
#ifdef SIQS_TIMING
      if (timing && worker->published_at != 0) {
        worker->result.profile.worker_parked_us += siqs_timing_elapsed(worker->published_at);
        worker->published_at = 0;
      }
#endif
      break;
    }
    gcd_task = worker->task == PSIQS_TASK_GCD;
#ifdef SIQS_TIMING
    if (timing) {
      if (worker->published_at != 0) {
        worker->result.profile.worker_parked_us += siqs_timing_elapsed(worker->published_at);
        worker->published_at = 0;
      }
      /* GCD work has its own job CPU/wall observations. Do not count it as
       * sieve activity, polynomials, or a sieve publication. */
      if (!gcd_task)
      { active_start = siqs_timing_now(); cpu_start = siqs_timing_cpu_now(); }
    }
#endif
    pthread_mutex_unlock(&pool->mutex);
    if (gcd_task) siqs_tlp_gcd_job_run(worker->gcd_job);
    else
      psiqs_sieve_family(worker);
#ifdef SIQS_TIMING
    if (timing
        && !gcd_task
       ) {
      worker->result.profile.worker_active_us += siqs_timing_elapsed(active_start);
      worker->result.profile.worker_cpu_us += siqs_timing_cpu_now() - cpu_start;
      worker->result.profile.worker_polynomials += worker->polynomials;
      ++worker->result.profile.publications;
    }
#endif
    pthread_mutex_lock(&pool->mutex);
    worker->state = PSIQS_READY;
#ifdef SIQS_TIMING
    if (timing) worker->published_at = siqs_timing_now();
#endif
    if (pool->queued >= pool->count)
      croak("PSIQS: completed-task queue overflow");
    pool->ready[pool->tail] = worker->index;
    if (++pool->tail == pool->count)
      pool->tail = 0;
    pool->queued++;
    pthread_cond_signal(&pool->done);
  }
  pthread_mutex_unlock(&pool->mutex);
  return NULL;
}

/* Stop assigning work, finish short polynomial prefixes of running families,
 * and cancel sieve jobs not yet begun. Assigned GCD jobs must finish, including
 * ones not yet begun, so the caller's final batch drain cannot wait forever.
 * Unrun family tails are not resumed on retry. */
static void psiqs_pool_join(psiqs_pool_t *pool) {
  uint32_t i;
  if (pool->live == 0)
    return;
  pthread_mutex_lock(&pool->mutex);
  pool->stop = 1;
  for (i = 0; i < pool->conditions; i++)
    if (pool->workers[i].started)
      pthread_cond_signal(&pool->workers[i].work);
  pthread_mutex_unlock(&pool->mutex);
  for (i = 0; i < pool->count; i++) {
    if (pool->workers[i].started) {
      int error = pthread_join(pool->workers[i].thread, NULL);
      if (error != 0)
        croak("PSIQS: pthread_join failed: %s", strerror(error));
      pool->workers[i].started = 0;
    }
  }
  pool->live = 0;
}

static void psiqs_pool_destroy(psiqs_pool_t *pool) {
  uint32_t i;
  if (pool == NULL)
    return;
  psiqs_pool_join(pool);
  psiqs_pool_unbind_gcd(pool);
  for (i = 0; i < pool->conditions; i++)
    pthread_cond_destroy(&pool->workers[i].work);
  pthread_cond_destroy(&pool->done);
  pthread_mutex_destroy(&pool->mutex);
  for (i = 0; i < pool->initialized; i++)
    psiqs_worker_clear(pool->workers + i);
  free(pool->ready);
  free(pool->workers);
  free(pool);
}

/* Creation failures reduce the pool; no jobs run until initialization ends.
 * If no worker can start, the caller uses the ordinary serial collector. */
static psiqs_pool_t *psiqs_pool_create(siqs_ctx_t *ctx) {
  psiqs_pool_t *pool = (psiqs_pool_t *)calloc(1, sizeof(*pool));
  uint32_t i;
  if (pool == NULL)
    return NULL;
  pool->count = ctx->nthreads;
  pool->master = ctx;
#ifdef SIQS_TIMING
  pool->timing_enabled = ctx->params.bits >= SIQS_TIMING_MIN_BITS;
#endif
  pool->workers = (psiqs_worker_t *)calloc(pool->count, sizeof(*pool->workers));
  pool->ready = (uint32_t *)malloc((size_t)pool->count * sizeof(*pool->ready));
  if (pool->workers == NULL || pool->ready == NULL)
    goto fail_mutex;
  if (pthread_mutex_init(&pool->mutex, NULL) != 0)
    goto fail_mutex;
  psiqs_pool_set_publication_interval(pool,
      ctx->params.max_large_primes == 3
        ? SIQS_TLP_WORKER_PUBLISH_POLYNOMIALS : PSIQS_PUBLICATION_POLYNOMIALS);
  if (pthread_cond_init(&pool->done, NULL) != 0)
    goto fail_done;
  for (i = 0; i < pool->count; i++) {
    psiqs_worker_t *worker = pool->workers + i;
    psiqs_worker_init(worker, ctx, i);
    pool->initialized++;
    worker->pool = pool;
    worker->index = i;
    if (pthread_cond_init(&worker->work, NULL) != 0) {
      psiqs_pool_destroy(pool);
      return NULL;
    }
    pool->conditions++;
  }
  for (i = 0; i < pool->count; i++) {
    psiqs_worker_t *worker = pool->workers + i;
    int error = pthread_create(&worker->thread, NULL, psiqs_pool_worker, worker);
    if (error == 0) {
      worker->started = 1;
      pool->live++;
    } else {
      fprintf(stderr, "PSIQS: pthread_create: %s; reducing worker pool\n",
              strerror(error));
      /* No thread owns this slot. Keep its condition for pool teardown, but
       * release scratch now; the ownership flag prevents a second clear. */
      psiqs_worker_clear(worker);
    }
  }
  if (pool->live == 0) {
    psiqs_pool_destroy(pool);
    return NULL;
  }
  if (ctx->params.max_large_primes == 3) {
    ctx->tlp_submit_gcd = psiqs_pool_submit_gcd;
    ctx->tlp_submit_data = pool;
  }
  return pool;

fail_done:
  pthread_mutex_destroy(&pool->mutex);
fail_mutex:
  free(pool->ready);
  free(pool->workers);
  free(pool);
  return NULL;
}

/* Only the caller assigns families, preserving the single-owner A hash/RNG. */
static int psiqs_pool_assign(psiqs_pool_t *pool, siqs_ctx_t *ctx,
                              siqs_poly_t *dispatch, psiqs_worker_t *worker,
                              uint32_t limit) {
  if (!worker->family_assigned ||
      worker->family_polynomials == worker->poly.b_limit) {
    if (!psiqs_assign_family(ctx, dispatch, worker, limit))
      return 0;
  } else {
    psiqs_prepare_publication(worker, limit);
  }
  pthread_mutex_lock(&pool->mutex);
  worker->task = PSIQS_TASK_SIEVE;
  worker->gcd_job = NULL;
  worker->state = PSIQS_WORK;
  pthread_cond_signal(&worker->work);
  pthread_mutex_unlock(&pool->mutex);
  return 1;
}

/* Taking a result acquires every write made by that worker before publication. */
static psiqs_worker_t *psiqs_pool_take(psiqs_pool_t *pool) {
  psiqs_worker_t *worker;
#ifdef SIQS_TIMING
  uint64_t start = pool->timing_enabled ? siqs_timing_now() : 0;
#endif
  pthread_mutex_lock(&pool->mutex);
  while (pool->queued == 0)
    pthread_cond_wait(&pool->done, &pool->mutex);
  worker = pool->workers + pool->ready[pool->head];
  if (++pool->head == pool->count)
    pool->head = 0;
  pool->queued--;
  worker->state = PSIQS_IDLE;
  pthread_mutex_unlock(&pool->mutex);
#ifdef SIQS_TIMING
  if (start) worker->result.profile.ready_wait_us += siqs_timing_elapsed(start);
#endif
  return worker;
}

/* A bounded, asynchronous A-family pool: no per-polynomial locks or graph
 * sharing. Poll stop/publication requests together every 32 polynomials.
 * Reserve polynomial budgets on assignment, refund unused work on
 * completion, and join every thread before the matrix solver can run. */
static int psiqs_collect_relations(siqs_ctx_t *ctx, siqs_poly_t *dispatch,
                                   uint32_t target,
                                   uint32_t *next_matrix_check,
                                   uint64_t *family_count,
                                   uint64_t *poly_count) {
  psiqs_pool_t *pool;
  uint32_t i, check_interval = ctx->params.fb_size / 128;
  uint64_t max_polynomials = siqs_polynomial_limit(&ctx->params);
  uint64_t report_step, next_report;
  uint32_t last_report_count = UINT32_MAX, active = 0;
  uint64_t last_report_polys = UINT64_MAX, remaining;
  int verbose = ctx->verbose, complete = 0, exhausted = 0, ready = 0;
  if (check_interval < SIQS_MATRIX_CHECK_MIN)
    check_interval = SIQS_MATRIX_CHECK_MIN;
  if (check_interval > SIQS_MATRIX_CHECK_MAX)
    check_interval = SIQS_MATRIX_CHECK_MAX;
  if (ctx->factor_found || (ctx->use_bids ? ctx->bids_stop :
      siqs_relation_target_reached(ctx, target)))
    return 1;
  if (*poly_count >= max_polynomials)
    return 0;
  pool = psiqs_pool_create(ctx);
  if (pool == NULL) {
    fprintf(stderr, "PSIQS: worker pool unavailable; using serial collection\n");
    if (ctx->use_bids) {
      siqs_collect_bids(ctx, dispatch, family_count, poly_count);
      return ctx->factor_found || ctx->bids_stop;
    }
    return siqs_collect_relations(ctx, dispatch, target, next_matrix_check,
                                  family_count, poly_count);
  }
  /* Use successfully created workers, not requested slots.  High core counts
   * otherwise make the shared collection-work clock excessively chatty. */
  report_step = siqs_progress_poly_step(ctx, pool->live);
  next_report = *poly_count + report_step;
  remaining = max_polynomials - *poly_count;
  for (i = 0; i < pool->count && remaining != 0; i++) {
    if (pool->workers[i].started) {
      uint32_t limit = dispatch->b_limit < remaining
                     ? dispatch->b_limit : (uint32_t)remaining;
      if (!psiqs_pool_assign(pool, ctx, dispatch, pool->workers + i, limit)) {
        exhausted = 1;
        break;
      }
      remaining -= pool->workers[i].limit;
      active++;
    }
  }
  while (active != 0) {
    psiqs_worker_t *worker = psiqs_pool_take(pool);
    int dispatched = 0;
    uint32_t unused = 0;
    active--;
    if (worker->task == PSIQS_TASK_GCD) {
      remaining += psiqs_release_gcd_worker(ctx, worker);
    } else {
      unused = worker->limit - worker->polynomials;
      remaining += unused;
      if (worker->polynomials != 0 &&
          worker->family_polynomials == worker->polynomials)
        (*family_count)++;
      *poly_count += worker->polynomials;
      pool->merging_worker = worker;
      pool->merging_dispatched = 0;
      psiqs_merge_worker(ctx, worker);
      dispatched = pool->merging_dispatched;
      pool->merging_worker = NULL;
      if (dispatched) {
        /* Keep this family's previous reservation while its executor does GCD.
         * Other workers must not spend its tail on newly assigned A families. */
        worker->gcd_reserved = unused;
        remaining -= unused;
        active++;
      }
      siqs_tlp_poll(ctx);
    }
    if (!ctx->use_bids && siqs_relation_count(ctx) >= *next_matrix_check && !ctx->factor_found) {
      uint32_t rows, columns;
      ready = siqs_matrix_ready(ctx, &rows, &columns);
      *next_matrix_check = siqs_relation_count(ctx) + check_interval;
      if (!ready && verbose > 2) {
        siqs_print_matrix_progress(ctx, rows, columns, ready);
      }
      if (ready) {
        complete = 1;
        break;
      }
    }
    if (verbose > 1 && *poly_count >= next_report) {
      siqs_print_polynomial_notice(ctx, *poly_count);
      siqs_print_relation_report(ctx, target, *poly_count);
      last_report_count = siqs_relation_count(ctx);
      last_report_polys = *poly_count;
      next_report = *poly_count + report_step;
    }
    if (ctx->factor_found || (ctx->use_bids ? ctx->bids_stop :
      siqs_relation_target_reached(ctx, target))) {
      complete = 1;
      break;
    }
    if (
        !dispatched &&
        remaining != 0 && (!exhausted ||
        worker->family_polynomials < worker->poly.b_limit)) {
      uint32_t limit = dispatch->b_limit < remaining
                     ? dispatch->b_limit : (uint32_t)remaining;
      if (psiqs_pool_assign(pool, ctx, dispatch, worker, limit)) {
        remaining -= worker->limit;
        active++;
      } else {
        exhausted = 1;
      }
    }
  }
  psiqs_pool_join(pool);
  /* No worker can still mutate these buffers.  Include every family that ran,
   * even if readiness was detected before its completion; canceled jobs have
   * no results.  Total executed polynomials cannot exceed reserved budgets. */
  for (i = 0; i < pool->count; i++) {
    psiqs_worker_t *worker = pool->workers + i;
    if (worker->state == PSIQS_READY) {
      if (worker->task == PSIQS_TASK_GCD) {
        (void)psiqs_release_gcd_worker(ctx, worker);
        continue;
      }
      if (worker->polynomials != 0 &&
          worker->family_polynomials == worker->polynomials)
        (*family_count)++;
      *poly_count += worker->polynomials;
      psiqs_merge_worker(ctx, worker);
    }
  }
#ifdef SIQS_TIMING
  for (i = 0; i < pool->count; i++) psiqs_merge_profile(ctx, pool->workers + i);
#endif
  siqs_tlp_flush(ctx);
  psiqs_pool_unbind_gcd(pool);
  if (ctx->params.max_large_primes == 3 && !ctx->factor_found) {
    uint32_t rows, columns;
    if (siqs_matrix_ready(ctx, &rows, &columns)) complete = ready = 1;
  }
  if (verbose > 0)
    siqs_print_polynomial_notice(ctx, *poly_count);
  if (ctx->factor_found || (ctx->use_bids ? ctx->bids_stop :
      siqs_relation_target_reached(ctx, target)))
    complete = 1;
  if (ready && verbose > 1) {
    uint32_t rows, columns;
    (void)siqs_matrix_ready(ctx, &rows, &columns);
    siqs_print_relation_report(ctx, target, *poly_count);
    last_report_count = siqs_relation_count(ctx);
    last_report_polys = *poly_count;
    siqs_print_matrix_progress(ctx, rows, columns, 1);
  }
  if (verbose > 1 &&
      (last_report_count != siqs_relation_count(ctx) || last_report_polys != *poly_count))
    siqs_print_relation_report(ctx, target, *poly_count);
  psiqs_pool_destroy(pool);
  return complete;
}
