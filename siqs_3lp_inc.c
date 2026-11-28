/* 3LP collection and cofactor completion. Guarded pairs reuse 2LP splitting.
 * The coordinator owns queues, prime blocks, GCD context and LP-row map.
 * An existing worker can temporarily execute the one GCD context; the caller
 * retains splitting, acceptance and all graph/journal updates.
 * Copyright (c) 2026 Dana Jacobsen. */

static void siqs_tlp_policy_init(siqs_ctx_t *ctx) {
  siqs_tlp_policy_t *p;
  mpz_t square, r2;
  if (ctx->params.max_large_primes != 3) return;
  if (ctx->tlp_policy != NULL) croak("SIQS 3LP: duplicate cofactor policy");
  p = siqs_calloc(1, sizeof(*p));
  mpz_inits(p->prime_limit, p->triple_min, p->gap_lower, square, r2, NULL);
  mpz_set_ui(p->triple_min, ctx->largest_fb_prime);
  mpz_nextprime(p->triple_min, p->triple_min);
  mpz_mul(p->prime_limit, p->triple_min, p->triple_min);
  mpz_pow_ui(p->triple_min, p->triple_min, 3);
  siqs_mpz_set_u64(p->gap_lower, ctx->params.large_prime_bound);
  mpz_mul(square, p->gap_lower, p->gap_lower);
  mpz_mul_ui(p->gap_lower, p->gap_lower, ctx->params.tlp_cutoff);
  if (mpz_cmp(p->gap_lower, square) > 0) mpz_set(p->gap_lower, square);
  /* Newly routed R2 pairs may lie above U*L in another configuration. */
  siqs_mpz_set_u64(r2, ctx->params.smooth_bound);
  if (mpz_cmp(p->gap_lower, r2) < 0)
    mpz_set(p->gap_lower, r2);
  mpz_clears(square, r2, NULL);
  ctx->tlp_policy = p;
}

static void siqs_tlp_policy_clear(siqs_ctx_t *ctx) {
  if (ctx->tlp_policy == NULL) return;
  mpz_clears(ctx->tlp_policy->prime_limit, ctx->tlp_policy->triple_min, ctx->tlp_policy->gap_lower, NULL);
  free(ctx->tlp_policy);
  ctx->tlp_policy = NULL;
}

static void siqs_tlp_set_bounds(siqs_ctx_t *ctx) {
  uint64_t pmax = ctx->largest_fb_prime;
  siqs_parameters_t *p = &ctx->params;
  mpz_t root;
  mpz_init(root);
  siqs_mpz_set_u64(ctx->tlp_upper, p->large_prime_bound);
  mpz_mul_ui(ctx->tlp_upper, ctx->tlp_upper, (unsigned long)pmax);
  mpz_mul_ui(ctx->tlp_upper, ctx->tlp_upper, (unsigned long)pmax);
  if (mpz_sizeinbase(ctx->tlp_upper, 2) > 128)
    croak("SIQS 3LP: R3 exceeds the pending-record envelope");
  p->tlp_smooth_bound = mpz_get_d(ctx->tlp_upper);
  mpz_root(root, ctx->tlp_upper, 3);
  mpz_mul_ui(root, root, SIQS_TLP_CUTOFF_NUMERATOR);
  mpz_fdiv_q_ui(root, root, SIQS_TLP_CUTOFF_DENOMINATOR);
  if (mpz_cmp_ui(root, pmax) <= 0 || mpz_cmp_ui(root, UINT32_MAX) > 0)
    croak("SIQS 3LP: prime cutoff outside (P,UINT32_MAX]");
  p->tlp_cutoff = (uint32_t)mpz_get_ui(root);
  mpz_clear(root);
  siqs_tlp_policy_init(ctx);
}

#ifdef PSIQS
/* One externally executed batch-GCD job; included by the threaded 3LP path.
 * The coordinator owns preparation, result consumption and lifetime. A pool
 * worker (or the coordinator's fallback) calls run exactly once per prepared
 * job. There is no thread creation. The ordinary task performs only GCD;
 * an optional callback owns the entire private residual-first pipeline.
 * Inputs, outputs and one mutable batch context are borrowed until wait or
 * clear succeeds. A separate producer buffer may be filled in the meantime.
 * Copyright (c) 2026 Dana Jacobsen. */

#include <pthread.h>

typedef struct {
  int status;
  uint64_t gcd_us, cpu_us;
} siqs_tlp_gcd_job_result_t;

enum {
  SIQS_TLP_GCD_JOB_IDLE,
  SIQS_TLP_GCD_JOB_PREPARED,
  SIQS_TLP_GCD_JOB_RUNNING,
  SIQS_TLP_GCD_JOB_DONE
};
enum {
  SIQS_TLP_GCD_JOB_MUTEX = 1U,
  SIQS_TLP_GCD_JOB_CONDITION = 2U,
  SIQS_TLP_GCD_JOB_INITIALIZED = 3U
};

typedef struct siqs_tlp_gcd_job {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  siqs_tlp_batch *batch;
  const mpz_t *inputs;
  mpz_t *gcds;
  size_t count;
  int (*callback)(void *);
  void *callback_data;
  siqs_tlp_gcd_job_result_t result;
  unsigned initialized;
  int state;
#ifdef SIQS_TIMING
  int timing_enabled;
#endif
} siqs_tlp_gcd_job_t;

/* Only the coordinator clears a job; clear must not race prepare/ready/wait.
 * An already running executor finishes before clear releases synchronization.
 * A prepared job has no executor yet, so return failure instead of waiting
 * forever. The caller must then arrange execution before releasing anything.
 * The executor touches no job memory after its final mutex unlock in run. */
static int siqs_tlp_gcd_job_clear(siqs_tlp_gcd_job_t *job) {
  if (job == NULL) return 1;
  if (job->initialized & SIQS_TLP_GCD_JOB_MUTEX) {
    if (pthread_mutex_lock(&job->mutex) != 0) return 0;
    if (job->state == SIQS_TLP_GCD_JOB_PREPARED) {
      pthread_mutex_unlock(&job->mutex);
      return 0;
    }
    while (job->state == SIQS_TLP_GCD_JOB_RUNNING) {
      if (!(job->initialized & SIQS_TLP_GCD_JOB_CONDITION) ||
          pthread_cond_wait(&job->changed, &job->mutex) != 0) {
        pthread_mutex_unlock(&job->mutex);
        return 0;
      }
    }
    pthread_mutex_unlock(&job->mutex);
  }
  if (job->initialized & SIQS_TLP_GCD_JOB_CONDITION) {
    if (pthread_cond_destroy(&job->changed) != 0) return 0;
    job->initialized &= ~SIQS_TLP_GCD_JOB_CONDITION;
  }
  if (job->initialized & SIQS_TLP_GCD_JOB_MUTEX) {
    if (pthread_mutex_destroy(&job->mutex) != 0) return 0;
    job->initialized &= ~SIQS_TLP_GCD_JOB_MUTEX;
  }
  memset(job, 0, sizeof(*job));
  return 1;
}

/* Initialize a fresh or cleared object, never an already-live job. Failure
 * leaves an object safe to clear; the caller still owns the batch context. */
static int siqs_tlp_gcd_job_init(siqs_tlp_gcd_job_t *job,
                                siqs_tlp_batch *batch, int timing_enabled) {
  if (job == NULL) return 0;
  memset(job, 0, sizeof(*job));
  if (batch == NULL) return 0;
  job->batch = batch;
#ifdef SIQS_TIMING
  job->timing_enabled = timing_enabled != 0;
#else
  (void)timing_enabled;
#endif
  if (pthread_mutex_init(&job->mutex, NULL) != 0) goto fail;
  job->initialized |= SIQS_TLP_GCD_JOB_MUTEX;
  if (pthread_cond_init(&job->changed, NULL) != 0) goto fail;
  job->initialized |= SIQS_TLP_GCD_JOB_CONDITION;
  return 1;
fail:
  (void)siqs_tlp_gcd_job_clear(job);
  return 0;
}

/* A DONE job remains busy until its result is consumed. Batch arguments are
 * validated by batch_gcd during run, and its exact status reaches the caller.
 * Publication through the mutex also makes all prior MPZ writes visible to
 * the executor; callers leave the borrowed arrays alone until completion. */
static int siqs_tlp_gcd_job_prepare(siqs_tlp_gcd_job_t *job,
    const mpz_t *inputs, mpz_t *gcds, size_t count) {
  int prepared = 0;
  if (job == NULL || job->initialized != SIQS_TLP_GCD_JOB_INITIALIZED) return 0;
  if (pthread_mutex_lock(&job->mutex) != 0) return 0;
  if (job->state == SIQS_TLP_GCD_JOB_IDLE) {
    job->inputs = inputs; job->gcds = gcds; job->count = count;
    job->callback = NULL; job->callback_data = NULL;
    memset(&job->result, 0, sizeof(job->result));
    job->state = SIQS_TLP_GCD_JOB_PREPARED;
    prepared = 1;
  }
  pthread_mutex_unlock(&job->mutex);
  return prepared;
}

#if SIQS_RESIDUAL_FIRST
/* The same ownership protocol also executes one whole residual-first job.
 * Its context and all referenced records remain private until wait succeeds. */
static int siqs_tlp_gcd_job_prepare_callback(siqs_tlp_gcd_job_t *job,
                                             int (*callback)(void *), void *data) {
  int prepared = 0;
  if (job == NULL || callback == NULL ||
      job->initialized != SIQS_TLP_GCD_JOB_INITIALIZED) return 0;
  if (pthread_mutex_lock(&job->mutex) != 0) return 0;
  if (job->state == SIQS_TLP_GCD_JOB_IDLE) {
    job->inputs = NULL; job->gcds = NULL; job->count = 0;
    job->callback = callback; job->callback_data = data;
    memset(&job->result, 0, sizeof(job->result));
    job->state = SIQS_TLP_GCD_JOB_PREPARED; prepared = 1;
  }
  pthread_mutex_unlock(&job->mutex); return prepared;
}
#endif

static void siqs_tlp_gcd_job_run(siqs_tlp_gcd_job_t *job) {
  siqs_tlp_gcd_job_result_t result;
  siqs_tlp_batch *batch;
  const mpz_t *inputs;
  mpz_t *gcds;
  size_t count;
  int (*callback)(void *); void *callback_data;
#ifdef SIQS_TIMING
  uint64_t wall_start = 0, cpu_start = 0;
  int timing_enabled;
#endif
  if (job == NULL || job->initialized != SIQS_TLP_GCD_JOB_INITIALIZED) return;
  if (pthread_mutex_lock(&job->mutex) != 0) return;
  if (job->state != SIQS_TLP_GCD_JOB_PREPARED) {
    pthread_mutex_unlock(&job->mutex);
    return;
  }
  batch = job->batch; inputs = job->inputs; gcds = job->gcds; count = job->count;
  callback = job->callback; callback_data = job->callback_data;
#ifdef SIQS_TIMING
  timing_enabled = job->timing_enabled;
#endif
  job->state = SIQS_TLP_GCD_JOB_RUNNING;
  pthread_mutex_unlock(&job->mutex);
  memset(&result, 0, sizeof(result));
#ifdef SIQS_TIMING
  if (timing_enabled) {
    wall_start = siqs_timing_now();
    cpu_start = siqs_timing_cpu_now();
  }
#endif
  result.status = callback != NULL ? callback(callback_data)
                  : siqs_tlp_batch_gcd(batch, inputs, gcds, count);
#ifdef SIQS_TIMING
  if (timing_enabled) {
    result.gcd_us = siqs_timing_now() - wall_start;
    result.cpu_us = siqs_timing_cpu_now() - cpu_start;
  }
#endif
  pthread_mutex_lock(&job->mutex);
  job->result = result;
  job->state = SIQS_TLP_GCD_JOB_DONE;
  pthread_cond_broadcast(&job->changed);
  pthread_mutex_unlock(&job->mutex);
}

static int siqs_tlp_gcd_job_ready(siqs_tlp_gcd_job_t *job) {
  int ready;
  if (job == NULL || job->initialized != SIQS_TLP_GCD_JOB_INITIALIZED) return 0;
  if (pthread_mutex_lock(&job->mutex) != 0) return 0;
  ready = job->state == SIQS_TLP_GCD_JOB_DONE;
  pthread_mutex_unlock(&job->mutex);
  return ready;
}

/* Acquiring the completion mutex makes every output limb visible before the
 * caller may read or recycle buffers. Idle/invalid calls leave result alone. */
static int siqs_tlp_gcd_job_wait(siqs_tlp_gcd_job_t *job,
                                 siqs_tlp_gcd_job_result_t *result) {
  int completed = 0;
  if (job == NULL || result == NULL ||
      job->initialized != SIQS_TLP_GCD_JOB_INITIALIZED) return 0;
  if (pthread_mutex_lock(&job->mutex) != 0) return 0;
  while (job->state == SIQS_TLP_GCD_JOB_PREPARED ||
         job->state == SIQS_TLP_GCD_JOB_RUNNING) {
    if (pthread_cond_wait(&job->changed, &job->mutex) != 0) {
      pthread_mutex_unlock(&job->mutex);
      return 0;
    }
  }
  if (job->state == SIQS_TLP_GCD_JOB_DONE) {
    *result = job->result;
    job->inputs = NULL; job->gcds = NULL; job->count = 0;
    job->callback = NULL; job->callback_data = NULL;
    job->state = SIQS_TLP_GCD_JOB_IDLE;
    completed = 1;
  }
  pthread_mutex_unlock(&job->mutex);
  return completed;
}
typedef struct {
  siqs_raw_relation_t **pending;
  mpz_t *inputs, *gcds;
  size_t count;
  siqs_raw_arena_t arena;
} siqs_tlp_flight_t;
#endif

struct siqs_tlp_state {
  siqs_tlp_prime_store *store;
  siqs_tlp_batch *batch;
  siqs_raw_relation_t **pending;
  mpz_t *inputs, *gcds;
  size_t count, capacity;
  int flushing;
#ifdef PSIQS
  siqs_tlp_flight_t flight;
  siqs_tlp_gcd_job_t gcd_job;
  int gcd_job_ready, flight_on_worker;
#endif
  mpz_t current, factor, quotient, product, parts[8];
};

static size_t siqs_tlp_pending_count(const siqs_ctx_t *ctx) {
  const struct siqs_tlp_state *s = ctx->tlp;
#if SIQS_RESIDUAL_FIRST
  if (ctx->rf != NULL) return siqs_rf_pending(ctx);
#endif
  if (s == NULL) return 0;
#ifdef PSIQS
  return s->count + s->flight.count;
#else
  return s->count;
#endif
}

#ifdef PSIQS
static void siqs_tlp_submit_full(siqs_ctx_t *ctx);
#endif

static void siqs_tlp_init_capacity(siqs_ctx_t *ctx, size_t capacity) {
  struct siqs_tlp_state *s;
  size_t i;
  if (ctx->params.max_large_primes != 3 || ctx->buffer_relations) return;
  s = (struct siqs_tlp_state *)siqs_calloc(1, sizeof(*s));
  ctx->tlp = s; s->capacity = capacity;
  if (capacity != 0) {
    s->inputs = (mpz_t *)siqs_malloc(siqs_array_size(capacity, sizeof(*s->inputs)));
    s->gcds = (mpz_t *)siqs_malloc(siqs_array_size(capacity, sizeof(*s->gcds)));
    for (i = 0; i < capacity; ++i) { mpz_init(s->inputs[i]); mpz_init(s->gcds[i]); }
  }
  mpz_inits(s->current, s->factor, s->quotient, s->product, NULL);
  for (i = 0; i < 8; ++i) mpz_init(s->parts[i]);
}

static void siqs_tlp_init(siqs_ctx_t *ctx) {
  size_t capacity = SIQS_TLP_BATCH_SIZE;
#if SIQS_RESIDUAL_FIRST
  /* Residual-first has its own queues. The coordinator uses the flush flag. */
  if (ctx->residual_first) capacity = 0;
#endif
  siqs_tlp_init_capacity(ctx, capacity);
  if (ctx->tlp != NULL && capacity != 0)
    ctx->tlp->pending = siqs_malloc(siqs_array_size(capacity, sizeof(*ctx->tlp->pending)));
}

static void siqs_tlp_clear(siqs_ctx_t *ctx) {
  struct siqs_tlp_state *s = ctx->tlp;
  size_t i;
  if (s == NULL) return;
#ifdef PSIQS
  /* The pool joins every assigned GCD task before context teardown. */
  if (s->gcd_job_ready > 0 && !siqs_tlp_gcd_job_clear(&s->gcd_job))
    croak("SIQS 3LP: unfinished GCD job during cleanup");
  for (i = 0; i < s->flight.count; ++i)
    mpz_clear(s->flight.pending[i]->y);
  if (s->flight.inputs != NULL) {
    for (i = 0; i < SIQS_TLP_BATCH_SIZE; ++i) {
      mpz_clear(s->flight.inputs[i]);
      mpz_clear(s->flight.gcds[i]);
    }
  }
  free(s->flight.pending); free(s->flight.inputs); free(s->flight.gcds);
  siqs_raw_arena_clear(&s->flight.arena);
#endif
  for (i = 0; i < s->count; ++i) mpz_clear(s->pending[i]->y);
  for (i = 0; i < s->capacity; ++i) { mpz_clear(s->inputs[i]); mpz_clear(s->gcds[i]); }
  for (i = 0; i < 8; ++i) mpz_clear(s->parts[i]);
  mpz_clears(s->current, s->factor, s->quotient, s->product, NULL);
  siqs_tlp_batch_destroy(s->batch);
  siqs_tlp_prime_store_destroy(s->store);
  free(s->inputs); free(s->gcds); free(s->pending);
  siqs_raw_arena_clear(&ctx->tlp_pending_arena);
  free(s); ctx->tlp = NULL;
}

/* lp3=0 marks an unresolved cofactor, packed into lp1/2 without uint128_t.
 * The separate batch arena is reset after every flush. Worker records instead
 * use their existing family arena and are copied during coordinator handoff. */
static siqs_raw_relation_t *siqs_tlp_pending_new(siqs_ctx_t *ctx,
    const mpz_t y, const siqs_factor_t *factors, uint32_t nfactors,
    const mpz_t rest) {
  uint64_t words[2] = {0, 0};
  size_t written;
  uint32_t i;
  siqs_raw_arena_t *arena = ctx->buffer_relations ? &ctx->raw_arena : &ctx->tlp_pending_arena;
  siqs_raw_relation_t *raw;
  if (mpz_sgn(rest) <= 0 || mpz_sizeinbase(rest, 2) > 128)
    croak("SIQS 3LP: pending cofactor exceeds checked 128-bit envelope");
  mpz_export(words, &written, -1, sizeof(uint64_t), 0, 0, rest);
  raw = (siqs_raw_relation_t *)siqs_raw_arena_alloc(arena, siqs_raw_allocation_size(nfactors));
  mpz_init_set(raw->y, y); raw->nfactors = nfactors;
#if SIQS_RESIDUAL_FIRST
  raw->rf_norm = NULL;
#endif
  raw->lp1 = words[0]; raw->lp2 = words[1]; raw->lp3 = 0;
  raw->factors.packed = (uint32_t *)(raw + 1);
  for (i = 0; i < nfactors; ++i)
    raw->factors.packed[i] = siqs_pack_raw_factor(factors[i].row, factors[i].exponent);
  return raw;
}

static void siqs_tlp_queue(siqs_ctx_t *ctx, siqs_raw_relation_t *raw) {
  struct siqs_tlp_state *s = ctx->tlp;
  uint64_t words[2] = {raw->lp1, raw->lp2};
  if (s == NULL || s->pending == NULL || raw->lp3 != 0 || s->count >= s->capacity)
    croak("SIQS 3LP: invalid queue ownership or capacity");
  s->pending[s->count] = raw;
  mpz_import(s->inputs[s->count], 2, -1, sizeof(uint64_t), 0, 0, words);
  if (++s->count == s->capacity) {
#ifdef PSIQS
    if (ctx->tlp_submit_gcd != NULL) {
      siqs_tlp_submit_full(ctx);
      return;
    }
#endif
    siqs_tlp_flush(ctx);
  }
}

/* All primality and splitting here are conditional on a positive batch GCD.
 * A miss is bounded; it never calls an unlimited factor() or retries curves.
 * Nested quiet SIQS inputs are <331 bits and cannot reenter this 3LP policy. */
static int siqs_tlp_complete(siqs_ctx_t *ctx, const mpz_t n,
                             const mpz_t gcd, uint64_t labels[3], uint32_t *count) {
  struct siqs_tlp_state *s = ctx->tlp;
  uint32_t pending = 0, i;
#ifdef SIQS_TIMING
  const int timing_enabled = ctx->params.bits >= SIQS_TIMING_MIN_BITS;
  uint64_t timing_start = 0;
#endif
  *count = 0;
  mpz_set(s->parts[pending++], gcd);
  if (mpz_cmp(gcd, n) != 0) {
    mpz_divexact(s->parts[pending++], n, gcd);
  }
  while (pending != 0) {
    uint64_t value;
    int split = 0, prime, square;
    mpz_set(s->current, s->parts[--pending]);
#ifdef SIQS_TIMING
    if (timing_enabled) timing_start = siqs_timing_now();
#endif
    /* Every piece divides a fully FB-stripped polynomial residual. With
     * Q=nextprime(P), a positive piece below Q^2 is prime or fails p>P. */
    prime = (ctx->tlp_policy != NULL &&
             mpz_cmp(s->current, ctx->tlp_policy->prime_limit) < 0) ||
            siqs_is_prob_prime(s->current);
#ifdef SIQS_TIMING
    if (timing_enabled) {
      ctx->result->profile.tlp_prime_us += siqs_timing_elapsed(timing_start);
      ++ctx->result->profile.tlp_prime_calls;
    }
#endif
    if (prime) {
      if (*count == 3 || !siqs_mpz_to_u64(s->current, &value) ||
          value <= ctx->largest_fb_prime || value > ctx->params.large_prime_bound)
        return 0;
      labels[(*count)++] = value;
      continue;
    }
    if (pending + *count + 2U > 3U) return 0;
#ifdef SIQS_TIMING
    if (timing_enabled) timing_start = siqs_timing_now();
#endif
    square = mpz_perfect_square_p(s->current);
    if (square) {
      mpz_sqrt(s->factor, s->current); split = 1;
    }
#ifdef SIQS_TIMING
    /* Include every square predicate, and sqrt when that predicate succeeds. */
    if (timing_enabled) {
      ctx->result->profile.tlp_square_us += siqs_timing_elapsed(timing_start);
      ++ctx->result->profile.tlp_square_calls;
    }
#endif
    /* Every 64-bit composite piece uses the same deterministic splitter,
     * whether it came from the U-product GCD or its quotient. */
    if (!square && siqs_mpz_to_u64(s->current, &value)) {
      uint64_t factor;
#ifdef SIQS_TIMING
      if (timing_enabled) timing_start = siqs_timing_now();
#endif
      factor = tinyecm64(value, 0);
      if (factor != 0) {
        siqs_mpz_set_u64(s->factor, factor);
        split = 1;
      }
#ifdef SIQS_TIMING
      if (timing_enabled) {
        ctx->result->profile.tlp_ecm_us += siqs_timing_elapsed(timing_start);
        ++ctx->result->profile.tlp_ecm_calls;
      }
#endif
    }
    if (split && mpz_cmp_ui(s->factor, 1) > 0 && mpz_cmp(s->factor, s->current) < 0 &&
        mpz_divisible_p(s->current, s->factor)) {
      mpz_divexact(s->quotient, s->current, s->factor);
      mpz_set(s->parts[pending++], s->factor);
      mpz_set(s->parts[pending++], s->quotient);
    } else {
      uint32_t nf, j;
      mpz_t *parts;
      int good;
#ifdef SIQS_TIMING
      uint32_t bits = 0;
      if (timing_enabled) {
        bits = (uint32_t)mpz_sizeinbase(s->current, 2);
        timing_start = siqs_timing_now();
      }
#endif
      /* tinyecm128(150,4000,5) before quiet SIQS saved about 5-10% of
       * post-U completion CPU, but the small end-to-end gain did not
       * justify the additional code and complexity. */
      /* Divisors of the residual are already free of factor-base primes. */
      parts = gmp_siqs(s->current, &nf, ctx->largest_fb_prime, 0, 0, 0);
      good = nf >= 2 && nf + pending + *count <= 3;
      mpz_set_ui(s->product, 1);
      for (j = 0; j < nf; ++j) mpz_mul(s->product, s->product, parts[j]);
      if (mpz_cmp(s->product, s->current)) croak("SIQS 3LP: invalid completion partition");
      for (j = 0; good && j < nf; ++j) {
        mpz_set(s->parts[pending++], parts[j]);
      }
      gmp_siqs_free(parts, nf);
#ifdef SIQS_TIMING
      /* Include the quiet solver's partition validation, copy, and release. */
      if (timing_enabled) {
        uint64_t elapsed = siqs_timing_elapsed(timing_start);
        siqs_profile_t *profile = &ctx->result->profile;
        profile->tlp_siqs_us += elapsed;
        ++profile->tlp_siqs_calls;
        if (bits < 56U) ++profile->tlp_siqs_small_calls;
        else if (bits <= 64U) ++profile->tlp_siqs_mid_calls;
        else {
          ++profile->tlp_siqs_wide_calls;
          profile->tlp_siqs_wide_us += elapsed;
        }
      }
#endif
      if (!good) return 0;
    }
  }
  mpz_set_ui(s->product, 1);
  for (i = 0; i < *count; ++i) {
    siqs_mpz_set_u64(s->factor, labels[i]); mpz_mul(s->product, s->product, s->factor);
  }
  if (mpz_cmp(s->product, n)) croak("SIQS 3LP: completed labels do not multiply to input");
  for (i = 0; i < *count; ++i) {
    uint32_t j;
    for (j = i + 1; j < *count; ++j) if (labels[j] < labels[i]) {
      uint64_t t = labels[i]; labels[i] = labels[j]; labels[j] = t;
    }
  }
  return *count >= 1 && *count <= 3;
}

static void siqs_tlp_ensure_store(siqs_ctx_t *ctx) {
  struct siqs_tlp_state *s = ctx->tlp;
#ifdef SIQS_TIMING
  const int timing_enabled = ctx->params.bits >= SIQS_TIMING_MIN_BITS;
  uint64_t timing_start = 0;
#endif
  if (s->store == NULL) {
#ifdef SIQS_TIMING
    if (timing_enabled) timing_start = siqs_timing_now();
#endif
    if (siqs_tlp_prime_store_create(&s->store, ctx->largest_fb_prime, ctx->params.tlp_cutoff) != SIQS_TLP_OK ||
        siqs_tlp_batch_create(&s->batch, s->store, 0) != SIQS_TLP_OK)
      croak("SIQS 3LP: shared prime-store or batch construction failed");
#ifdef SIQS_TIMING
    if (timing_enabled)
      ctx->result->profile.tlp_setup_us += siqs_timing_elapsed(timing_start);
#endif
  }
}

/* Only the coordinator calls completion, updates counters, and accepts rows.
 * GCD inputs and outputs belong to this buffer until its job is acknowledged. */
static void siqs_tlp_consume(siqs_ctx_t *ctx,
    siqs_raw_relation_t **records, mpz_t *inputs, mpz_t *gcds,
    size_t count, siqs_raw_arena_t *arena) {
  size_t i;
#ifdef SIQS_TIMING
  const int timing_enabled = ctx->params.bits >= SIQS_TIMING_MIN_BITS;
  uint64_t timing_start = 0;
  uint64_t results_start = timing_enabled ? siqs_timing_now() : 0;
#endif
  for (i = 0; i < count; ++i) {
    siqs_raw_relation_t *pending = records[i];
    if (mpz_cmp_ui(gcds[i], 1) == 0) {
      ++ctx->tlp_gcd_one;
    } else {
      uint64_t labels[3]; uint32_t nlabels;
      int completed;
      if (mpz_cmp(gcds[i], inputs[i]) == 0) ++ctx->tlp_gcd_whole;
      else ++ctx->tlp_gcd_proper;
#ifdef SIQS_TIMING
      if (timing_enabled) timing_start = siqs_timing_now();
#endif
      completed = siqs_tlp_complete(ctx, inputs[i], gcds[i], labels, &nlabels);
#ifdef SIQS_TIMING
      /* Include failed completions and every early return from completion. */
      if (timing_enabled)
        ctx->result->profile.tlp_complete_us += siqs_timing_elapsed(timing_start);
#endif
      if (completed) {
        uint32_t j;
        siqs_factor_t *factors;
        siqs_raw_relation_t *raw;
#ifdef SIQS_TIMING
        if (timing_enabled) timing_start = siqs_timing_now();
#endif
        factors = siqs_eval_reserve_factors(ctx, pending->nfactors);
        for (j = 0; j < pending->nfactors; ++j) {
          factors[j].row = siqs_raw_factor_row(pending, j);
          factors[j].exponent = siqs_raw_factor_exponent(pending, j);
        }
        raw = siqs_raw_relation_new3(ctx, pending->y, factors, pending->nfactors,
            nlabels == 1 ? 1 : labels[0], nlabels == 1 ? labels[0] : labels[1],
            nlabels == 3 ? labels[2] : 1);
        ++ctx->tlp_completed;
        siqs_accept_raw_relation(ctx, raw);
#ifdef SIQS_TIMING
        if (timing_enabled)
          ctx->result->profile.tlp_accept_us += siqs_timing_elapsed(timing_start);
#endif
      } else ++ctx->tlp_rejected;
    }
    mpz_clear(pending->y);
  }
  siqs_raw_arena_clear(arena);
#ifdef SIQS_TIMING
  /* Inclusive: completion, acceptance/LP combination, and pending cleanup. */
  if (timing_enabled)
    ctx->result->profile.tlp_results_us += siqs_timing_elapsed(results_start);
#endif
}

#ifdef PSIQS
/* Reap a finished flight without touching the independent producer buffer.
 * Holding flushing suppresses journal recursion while consuming its results. */
static void siqs_tlp_finish_job(siqs_ctx_t *ctx, int wait) {
  struct siqs_tlp_state *s = ctx->tlp;
  siqs_tlp_gcd_job_result_t result;
#ifdef SIQS_TIMING
  uint64_t wait_start = 0;
#endif
  if (s == NULL || s->flight.count == 0 || s->flushing) return;
  if (!wait && !siqs_tlp_gcd_job_ready(&s->gcd_job)) return;
  s->flushing = 1;
#ifdef SIQS_TIMING
  if (ctx->params.bits >= SIQS_TIMING_MIN_BITS) wait_start = siqs_timing_now();
#endif
  if (!siqs_tlp_gcd_job_wait(&s->gcd_job, &result) || result.status != SIQS_TLP_OK)
    croak("SIQS 3LP: worker batch GCD failed");
#ifdef SIQS_TIMING
  if (wait_start) {
    ctx->result->profile.tlp_gcd_wait_us += siqs_timing_elapsed(wait_start);
    ctx->result->profile.tlp_gcd_us += result.gcd_us;
    ctx->result->profile.tlp_gcd_cpu_us += result.cpu_us;
    if (s->flight_on_worker) {
      ctx->result->profile.tlp_worker_gcd_cpu_us += result.cpu_us;
      ++ctx->result->profile.tlp_worker_gcd_jobs;
    }
  }
#endif
  ++ctx->tlp_batches;
  siqs_tlp_consume(ctx, s->flight.pending, s->flight.inputs, s->flight.gcds,
                   s->flight.count, &s->flight.arena);
  s->flight.count = 0;
  s->flight_on_worker = 0;
  s->flushing = 0;
  if (ctx->journal != NULL) sj_maybe_flush(ctx);
}

static void siqs_tlp_submit_full(siqs_ctx_t *ctx) {
  struct siqs_tlp_state *s = ctx->tlp;
  siqs_raw_relation_t **pending;
  mpz_t *values;
  siqs_raw_arena_t arena;
  size_t i;
  int timing_enabled = 0;
  if (s->count != SIQS_TLP_BATCH_SIZE || s->flushing)
    croak("SIQS 3LP: invalid full-batch submission");
  /* Two buffers bound the pipeline. Wait only if both have filled. */
  siqs_tlp_finish_job(ctx, 1);
  siqs_tlp_ensure_store(ctx);
  if (s->gcd_job_ready == 0) {
#ifdef SIQS_TIMING
    timing_enabled = ctx->params.bits >= SIQS_TIMING_MIN_BITS;
#endif
    s->gcd_job_ready = siqs_tlp_gcd_job_init(&s->gcd_job, s->batch,
                                            timing_enabled) ? 1 : -1;
  }
  if (s->gcd_job_ready < 0) {
    siqs_tlp_flush(ctx);
    return;
  }
  if (s->flight.pending == NULL) {
    s->flight.pending = (siqs_raw_relation_t **)siqs_malloc(
        siqs_array_size(SIQS_TLP_BATCH_SIZE, sizeof(*s->flight.pending)));
    s->flight.inputs = (mpz_t *)siqs_malloc(
        siqs_array_size(SIQS_TLP_BATCH_SIZE, sizeof(mpz_t)));
    s->flight.gcds = (mpz_t *)siqs_malloc(
        siqs_array_size(SIQS_TLP_BATCH_SIZE, sizeof(mpz_t)));
    for (i = 0; i < SIQS_TLP_BATCH_SIZE; ++i) {
      mpz_init(s->flight.inputs[i]); mpz_init(s->flight.gcds[i]);
    }
  }
  pending = s->pending; s->pending = s->flight.pending; s->flight.pending = pending;
  values = s->inputs; s->inputs = s->flight.inputs; s->flight.inputs = values;
  values = s->gcds; s->gcds = s->flight.gcds; s->flight.gcds = values;
  arena = ctx->tlp_pending_arena;
  ctx->tlp_pending_arena = s->flight.arena; s->flight.arena = arena;
  s->flight.count = s->count; s->count = 0;
  if (!siqs_tlp_gcd_job_prepare(&s->gcd_job, s->flight.inputs,
                               s->flight.gcds, s->flight.count))
    croak("SIQS 3LP: GCD context already in use");
  s->flight_on_worker = ctx->tlp_submit_gcd != NULL &&
      ctx->tlp_submit_gcd(ctx->tlp_submit_data, &s->gcd_job);
  if (!s->flight_on_worker) {
    /* Non-pool callers and a second submission in the same publication keep
     * the synchronous behavior, using the same one mutable GCD context. */
    siqs_tlp_gcd_job_run(&s->gcd_job);
    siqs_tlp_finish_job(ctx, 1);
  }
}
#endif

#ifdef PSIQS
static void siqs_tlp_poll(siqs_ctx_t *ctx) {
#if SIQS_RESIDUAL_FIRST
  if (ctx->rf != NULL) { siqs_rf_poll(ctx); return; }
#endif
  siqs_tlp_finish_job(ctx, 0);
}
#endif

/* Strong drain for solver, journal, stop and serial callers. An assigned GCD
 * task must finish even when its worker pool has received a stop request. */
static void siqs_tlp_flush(siqs_ctx_t *ctx) {
  struct siqs_tlp_state *s = ctx->tlp;
#if SIQS_RESIDUAL_FIRST
  if (ctx->rf != NULL) { siqs_rf_flush(ctx); return; }
#endif
#ifdef SIQS_TIMING
  const int timing_enabled = ctx->params.bits >= SIQS_TIMING_MIN_BITS;
  uint64_t start = 0, cpu_start = 0;
#endif
  if (s == NULL || s->flushing) return;
#ifdef PSIQS
  siqs_tlp_finish_job(ctx, 1);
#endif
  if (s->count == 0) return;
  s->flushing = 1;
  siqs_tlp_ensure_store(ctx);
#ifdef SIQS_TIMING
  if (timing_enabled) { start = siqs_timing_now(); cpu_start = siqs_timing_cpu_now(); }
#endif
  if (siqs_tlp_batch_gcd(s->batch, s->inputs, s->gcds, s->count) != SIQS_TLP_OK)
    croak("SIQS 3LP: batch GCD failed");
#ifdef SIQS_TIMING
  if (timing_enabled) {
    ctx->result->profile.tlp_gcd_us += siqs_timing_elapsed(start);
    ctx->result->profile.tlp_gcd_cpu_us += siqs_timing_cpu_now() - cpu_start;
  }
#endif
  ++ctx->tlp_batches;
  siqs_tlp_consume(ctx, s->pending, s->inputs, s->gcds, s->count,
                   &ctx->tlp_pending_arena);
  s->count = 0;
  s->flushing = 0;
  if (ctx->journal != NULL) sj_maybe_flush(ctx);
}

/* Smooth/single-LP fast paths remain local. Ordinary at-most-two-prime
 * residuals may use the existing R2 splitter. Unresolved cases retain their
 * batch opportunity; all other eligible residuals keep the original path. */
/* Return accepted(1), needs fixed-U batch(0), or rejected(-1). */
static int siqs_tlp_classify(siqs_ctx_t *ctx, const mpz_t rest,
                            uint64_t labels[3], uint32_t *count) {
  uint64_t value, p2 = (uint64_t)ctx->largest_fb_prime * ctx->largest_fb_prime;
  *count = 0;
  if (mpz_cmp_ui(rest, 1) == 0) return 1;
  if (siqs_mpz_to_u64(rest, &value) && value < p2) {
    if (value <= ctx->largest_fb_prime || value > ctx->params.large_prime_bound) return -1;
    labels[0] = value; *count = 1; return 1;
  }
  if (mpz_cmp(rest, ctx->tlp_upper) > 0) return -1;
  if (ctx->tlp_policy != NULL) {
    const siqs_tlp_policy_t *p = ctx->tlp_policy;
    if (mpz_cmp(rest, p->gap_lower) > 0 &&
        mpz_cmp(rest, p->triple_min) < 0) {
      ++ctx->tlp_gap_rejected; return -1;
    }
    if (siqs_mpz_to_u64(rest, &value) &&
        value <= ctx->params.smooth_bound && mpz_cmp(rest, p->triple_min) < 0) {
      uint64_t first, second; siqs_cofactor_outcome_t outcome;
      ++ctx->tlp_r2_attempts;
      outcome = siqs_resolve_cofactor_outcome(ctx, rest, &first, &second);
      if (outcome == SIQS_COFACTOR_RESOLVED) {
        ++ctx->tlp_r2_resolved;
        if (first == 1) { labels[0] = second; *count = 1; }
        else { labels[0] = first; labels[1] = second; *count = 2; }
        return 1;
      }
      if (outcome == SIQS_COFACTOR_REJECTED) { ++ctx->tlp_r2_rejected; return -1; }
      ++ctx->tlp_r2_fallback;
    }
  }
  ++ctx->tlp_candidates; return 0;
}

static siqs_raw_relation_t *siqs_tlp_labeled_raw(siqs_ctx_t *ctx,
    const mpz_t y, const siqs_factor_t *factors, uint32_t nfactors,
    const uint64_t labels[3], uint32_t count) {
  return siqs_raw_relation_new3(ctx, y, factors, nfactors,
      count >= 2 ? labels[0] : 1,
      count == 1 ? labels[0] : count >= 2 ? labels[1] : 1,
      count == 3 ? labels[2] : 1);
}

static void siqs_resolve_cofactor_3lp(siqs_ctx_t *ctx, const mpz_t rest,
    const mpz_t y, const siqs_factor_t *factors, uint32_t nfactors) {
  uint64_t labels[3]; uint32_t count; siqs_raw_relation_t *raw;
  int status = siqs_tlp_classify(ctx, rest, labels, &count);
  if (status < 0) return;
  if (status == 0) {
    raw = siqs_tlp_pending_new(ctx, y, factors, nfactors, rest);
    if (ctx->buffer_relations) siqs_store_raw(ctx, raw);
    else siqs_tlp_queue(ctx, raw);
    return;
  }
  raw = siqs_tlp_labeled_raw(ctx, y, factors, nfactors, labels, count);
  if (ctx->buffer_relations) siqs_store_raw(ctx, raw);
  else siqs_accept_raw_relation(ctx, raw);
}

#if SIQS_RESIDUAL_FIRST
/* Exact residual-first collection; private to this SIQS implementation.
 * The coordinator owns one producer buffer and bounded worker-pool slots.
 * Each slot owns its resolver, GMP scratch and persistent LP candidate queue.
 * Only immutable FB tables/products are borrowed. No job modifies the graph,
 * factor partition, journal, or a sieve worker's scratch/result buffer.
 * Copyright (c) 2026 Dana Jacobsen. */

#ifndef PSIQS
# error "Residual-first currently requires the standalone worker-GCD build"
#endif

typedef struct {
  siqs_raw_relation_t *packet;
  uint64_t labels[3];
  uint32_t count;
} siqs_rf_accepted_t;

struct siqs_rf_state {
  struct siqs_rf_pool *pool;
  siqs_ctx_t *owner;
  siqs_raw_relation_t **flight, **lp_pending;
  size_t flight_count, lp_count, published_lp_count;
  siqs_raw_arena_t flight_arena, lp_arena;
  siqs_tlp_prime_store *fb_store;
  siqs_tlp_batch *fb_batch;
  siqs_tlp_batch *fb_tail, *lp_tail;
  mpz_t *inputs, *gcds, *tree;
  size_t leaves;
  struct {
    mpz_t aggregate, distinct;
    mpz_t *tree, *scratch;
    size_t tree_alloc, depths;
    int initialized;
  } recovery;
  uint32_t *selected;
  size_t selected_count, selected_alloc;
  siqs_rf_accepted_t *direct, *lp_accepted;
  size_t direct_alloc, lp_accepted_alloc;
  siqs_ctx_t resolver;
  siqs_factor_array_t result;
  siqs_tlp_gcd_job_t job;
  uint64_t ticket;
  int running, drain_lp, on_worker;
};

struct siqs_rf_pool {
  siqs_raw_relation_t **pending;
  size_t count, fb_capacity, lp_capacity;
  siqs_raw_arena_t pending_arena;
  siqs_tlp_prime_store *fb_store, *lp_store;
  mpz_t *tree;
  size_t leaves;
  struct siqs_rf_state **slots;
  unsigned job_count, job_limit, next_job, active_jobs, peak_active_jobs;
#if !SIQS_RF_ATOMICS
  pthread_mutex_t counter_mutex;
#endif
  uint64_t next_ticket;
  uint64_t inputs_done, jobs_done, recovered, short_jobs;
#ifdef SIQS_RF_TEST_HOOKS
  void (*started)(void *);
  void *started_data;
#endif
};

static siqs_raw_relation_t *siqs_rf_packet_new(siqs_raw_arena_t *arena,
    const mpz_t y, const siqs_factor_t *known, uint32_t count,
    const mpz_t norm, mpz_srcptr distinct) {
  size_t base = (siqs_raw_allocation_size(count) + 7U) & ~(size_t)7U;
  siqs_raw_relation_t *p = siqs_raw_arena_alloc(arena,
      siqs_size_add(base, sizeof(siqs_rf_norm_t)));
  uint32_t i;
  mpz_init_set(p->y, y); p->nfactors = count;
  p->lp1 = p->lp2 = 1; p->lp3 = 0;
  p->factors.packed = (uint32_t *)(p + 1);
  for (i = 0; i < count; ++i)
    p->factors.packed[i] = siqs_pack_raw_factor(known[i].row, known[i].exponent);
  p->rf_norm = (siqs_rf_norm_t *)((unsigned char *)p + base);
  mpz_init_set(p->rf_norm->norm, norm);
  mpz_init(p->rf_norm->distinct);
  if (distinct != NULL) mpz_set(p->rf_norm->distinct, distinct);
  return p;
}

static void siqs_rf_packet_clear(siqs_raw_relation_t *p) {
  mpz_clear(p->y); mpz_clear(p->rf_norm->norm);
  mpz_clear(p->rf_norm->distinct);
}

static int siqs_rf_factor_cmp(const void *a, const void *b) {
  uint32_t x = ((const siqs_factor_t *)a)->row;
  uint32_t y = ((const siqs_factor_t *)b)->row;
  return (x > y) - (x < y);
}

static uint32_t siqs_rf_coalesce(siqs_factor_t *factors, uint32_t count) {
  uint32_t i, out = 0;
  qsort(factors, count, sizeof(*factors), siqs_rf_factor_cmp);
  for (i = 0; i < count; ++i) {
    if (out != 0 && factors[out-1].row == factors[i].row)
      factors[out-1].exponent += factors[i].exponent;
    else factors[out++] = factors[i];
  }
  return out;
}

/* One callback owns a resolver slot at a time. Keep GMP scratch by
 * recursion depth, so a child never overwrites its parent's pending divisor.
 * The subset tree retains initialized storage; every active leaf, including
 * padding, is reset before rebuilding it for the next accepted batch. */
static void siqs_rf_recovery_workspace(struct siqs_rf_state *s) {
  size_t i, width = s->leaves, depths = 1;
  if (s->recovery.initialized) return;
  while (width > 1U) { width /= 2U; ++depths; }
  s->recovery.depths = depths;
  s->recovery.scratch = siqs_malloc(
      siqs_array_size(2U*depths, sizeof(*s->recovery.scratch)));
  for (i = 0; i < 2U*depths; ++i) mpz_init(s->recovery.scratch[i]);
  mpz_inits(s->recovery.aggregate, s->recovery.distinct, NULL);
  s->recovery.initialized = 1;
}

static void siqs_rf_recovery_clear(struct siqs_rf_state *s) {
  size_t i;
  if (!s->recovery.initialized) return;
  for (i = 0; i < 2U*s->recovery.depths; ++i)
    mpz_clear(s->recovery.scratch[i]);
  for (i = 0; i < s->recovery.tree_alloc; ++i)
    mpz_clear(s->recovery.tree[i]);
  mpz_clears(s->recovery.aggregate, s->recovery.distinct, NULL);
  free(s->recovery.scratch); free(s->recovery.tree);
  memset(&s->recovery, 0, sizeof(s->recovery));
}

/* A null q collects the union's sorted FB indices. Otherwise remove
 * all powers from the norm and append its recovered factor/exponent. */
static void siqs_rf_recovery_emit(struct siqs_rf_state *s, uint32_t i,
    mpz_ptr q, siqs_factor_t *factors, uint32_t *count) {
  if (q != NULL) {
    factors[*count].row = i+1U;
    factors[(*count)++].exponent =
        (uint32_t)siqs_remove_ui(q, s->owner->prime[i]);
  } else {
    if (s->selected_count == s->selected_alloc) {
      s->selected_alloc = s->selected_alloc ? 2U*s->selected_alloc : 1024U;
      s->selected = siqs_realloc(s->selected,
          siqs_array_size(s->selected_alloc, sizeof(*s->selected)));
    }
    s->selected[s->selected_count++] = i;
  }
}

/* distinct is squarefree even when q contains repeated powers. Shrink
 * it as primes are found so a sparse leaf stops as soon as it is complete. */
static void siqs_rf_recovery_leaf(struct siqs_rf_state *s,
    size_t first, size_t end, int full, size_t depth, const mpz_t distinct,
    mpz_ptr q, siqs_factor_t *factors, uint32_t *count) {
  size_t j;
  uint64_t remaining;
  uint32_t i, p;
  mpz_ptr rest = s->recovery.scratch[2U*depth];
  if (first >= end) return;
  if (siqs_mpz_to_u64(distinct, &remaining)) {
    for (j = first; j < end && remaining != 1U; ++j) {
      i = full ? (uint32_t)j : s->selected[j];
      p = s->owner->prime[i];
      if (remaining % p == 0) {
        siqs_rf_recovery_emit(s, i, q, factors, count);
        remaining /= p;
      }
    }
    if (remaining != 1U) croak("SIQS residual-first: incomplete leaf");
  } else {
    mpz_set(rest, distinct);
    for (j = first; j < end && mpz_cmp_ui(rest, 1) != 0; ++j) {
      i = full ? (uint32_t)j : s->selected[j];
      p = s->owner->prime[i];
      if (mpz_divisible_ui_p(rest, p)) {
        siqs_rf_recovery_emit(s, i, q, factors, count);
        mpz_divexact_ui(rest, rest, p);
      }
    }
    if (mpz_cmp_ui(rest, 1) != 0)
      croak("SIQS residual-first: incomplete large leaf");
  }
}

/* full indexes the immutable FB tree; otherwise index the accepted
 * batch's selected-prime tree. Each divisor belongs to the current node. */
static void siqs_rf_recovery_walk(struct siqs_rf_state *s, mpz_t *tree,
    size_t leaves, size_t node, size_t depth, const mpz_t distinct,
    mpz_ptr q, siqs_factor_t *factors, uint32_t *count, int full) {
  mpz_ptr left, right;
  if (mpz_cmp_ui(distinct, 1) == 0) return;
  if (depth >= s->recovery.depths)
    croak("SIQS residual-first: scratch depth");
  /* An exact match to an FB prime finishes this whole branch. The divisor
   * belongs to this node, even though the lookup uses the full index list. */
  if (mpz_cmp_ui(distinct, s->owner->largest_fb_prime) <= 0) {
    uint32_t value = (uint32_t)mpz_get_ui(distinct);
    size_t lo = full ? s->owner->params.sieve_start : 0;
    size_t hi = full ? s->owner->params.fb_size : s->selected_count;
    size_t limit = hi;
    while (lo < hi) {
      size_t mid = lo + (hi-lo)/2U;
      uint32_t i = full ? (uint32_t)mid : s->selected[mid];
      if (s->owner->prime[i] < value) lo = mid+1U;
      else hi = mid;
    }
    if (lo < limit) {
      uint32_t i = full ? (uint32_t)lo : s->selected[lo];
      if (s->owner->prime[i] == value) {
        siqs_rf_recovery_emit(s, i, q, factors, count);
        return;
      }
    }
  }
  if (node >= leaves) {
    size_t first = (node-leaves)*32U, end;
    if (full) first += s->owner->params.sieve_start;
    end = first+32U;
    if (full) {
      if (end > s->owner->params.fb_size) end = s->owner->params.fb_size;
    } else if (end > s->selected_count) end = s->selected_count;
    siqs_rf_recovery_leaf(s, first, end, full, depth, distinct,
                          q, factors, count);

    return;
  }
  left = s->recovery.scratch[2U*depth];
  right = s->recovery.scratch[2U*depth+1U];
  mpz_gcd(left, distinct, tree[2U*node]);
  if (mpz_cmp_ui(left, 1) == 0) {
    siqs_rf_recovery_walk(s, tree, leaves, 2U*node+1U, depth+1U,
                           distinct, q, factors, count, full);
    return;
  }
  if (mpz_cmp(left, distinct) == 0) {
    siqs_rf_recovery_walk(s, tree, leaves, 2U*node, depth+1U,
                           distinct, q, factors, count, full);
    return;
  }
  mpz_divexact(right, distinct, left);
  siqs_rf_recovery_walk(s, tree, leaves, 2U*node, depth+1U,
                         left, q, factors, count, full);
  siqs_rf_recovery_walk(s, tree, leaves, 2U*node+1U, depth+1U,
                         right, q, factors, count, full);
}

/* Reconstruct FB factors only for accepted norms, and verify that removing
 * them leaves exactly the accepted LP product. */
static void siqs_rf_recover(struct siqs_rf_state *s,
                             siqs_rf_accepted_t *accepted, size_t count) {
  siqs_ctx_t *ctx = &s->resolver;
  mpz_t *tree;
  size_t j, leaves = 1;
#ifdef SIQS_TIMING
  uint64_t start = siqs_timing_now();
#endif
  if (count == 0) return;
  siqs_rf_recovery_workspace(s);
  mpz_set_ui(s->recovery.aggregate, 1);
  for (j = 0; j < count; ++j)
    mpz_mul(s->recovery.aggregate, s->recovery.aggregate,
              accepted[j].packet->rf_norm->distinct);
  mpz_gcd(s->recovery.distinct, s->recovery.aggregate, s->tree[1]);
  s->selected_count = 0;
  siqs_rf_recovery_walk(s, s->tree, s->leaves, 1, 0,
      s->recovery.distinct, NULL, NULL, NULL, 1);
  while (leaves < (s->selected_count+31U)/32U) leaves *= 2U;
  if (2U*leaves > s->recovery.tree_alloc) {
    size_t old_alloc = s->recovery.tree_alloc;
    s->recovery.tree = siqs_realloc(s->recovery.tree,
        siqs_array_size(2U*leaves, sizeof(*s->recovery.tree)));
    for (j = old_alloc; j < 2U*leaves; ++j)
      mpz_init(s->recovery.tree[j]);
    s->recovery.tree_alloc = 2U*leaves;
  }
  tree = s->recovery.tree;
  /* Reset every active leaf, including padding, after size changes. */
  for (j = leaves; j < 2U*leaves; ++j) mpz_set_ui(tree[j], 1);
  for (j = 0; j < s->selected_count; ++j)
    mpz_mul_ui(tree[leaves+j/32U], tree[leaves+j/32U],
                  s->owner->prime[s->selected[j]]);
  for (j = leaves; j-- > 1;)
    mpz_mul(tree[j], tree[2U*j], tree[2U*j+1U]);
  for (j = 0; j < count; ++j) {
    siqs_rf_accepted_t *a = accepted+j;
    siqs_raw_relation_t *p = a->packet, *raw;
    uint32_t i, used = p->nfactors;
    size_t bits = mpz_sizeinbase(p->rf_norm->norm, 2);
    siqs_factor_t *factors;
    if (bits > UINT32_MAX - p->nfactors - 1U)
      croak("SIQS residual-first: norm too large for factors");
    factors = siqs_eval_reserve_factors(ctx, p->nfactors+(uint32_t)bits+1U);
    for (i = 0; i < p->nfactors; ++i) {
      factors[i].row = siqs_raw_factor_row(p, i);
      factors[i].exponent = siqs_raw_factor_exponent(p, i);
    }
    mpz_set(ctx->eval.q, p->rf_norm->norm);
    siqs_rf_recovery_walk(s, tree, leaves, 1, 0, p->rf_norm->distinct,
        ctx->eval.q, factors, &used, 0);
    mpz_set_ui(ctx->eval.y, 1);
    for (i = 0; i < a->count; ++i) {
      siqs_mpz_set_u64(ctx->eval.rest, a->labels[i]);
      mpz_mul(ctx->eval.y, ctx->eval.y, ctx->eval.rest);
    }
    if (mpz_cmp(ctx->eval.q, ctx->eval.y) != 0)
      croak("SIQS residual-first: incomplete FB recovery");
    used = siqs_rf_coalesce(factors, used);
    raw = siqs_tlp_labeled_raw(ctx, p->y, factors, used, a->labels, a->count);
    siqs_store_raw(ctx, raw);
  }
#ifdef SIQS_TIMING
  ctx->result->profile.rf_recover_us += siqs_timing_elapsed(start);
  ctx->result->profile.rf_accepted += count;
#endif
}

/* Small final jobs must not pad/multiply all 32768 product-tree leaves.
 * Keep at most one small scratch context for each immutable prime store. */
static siqs_tlp_batch *siqs_rf_batch(struct siqs_rf_state *s, int lp, size_t count) {
  siqs_tlp_batch *full = lp ? s->resolver.tlp->batch : s->fb_batch;
  siqs_tlp_batch **tail = lp ? &s->lp_tail : &s->fb_tail;
  const siqs_tlp_prime_store *store = lp ? s->resolver.tlp->store : s->fb_store;
  size_t capacity = 1, cached;
  size_t limit = lp ? s->pool->lp_capacity : s->pool->fb_capacity;
  if (count == 0 || count >= limit/4U) {
    if (full == NULL) {
      if (siqs_tlp_batch_create(&s->resolver.tlp->batch, store, limit) != SIQS_TLP_OK)
        croak("SIQS residual-first: LP scratch allocation");
      full = s->resolver.tlp->batch;
    }
    /* FB screening has finished before LP splitting starts, including an LP
     * flush inside the FB result loop. Equal capacities can reuse one tree.
     * Rebinding changes only this slot's private scratch, not either store. */
    full->store = store;
    return full;
  }
  while (capacity < count) capacity *= 2U;
  cached = siqs_tlp_batch_capacity(*tail);
  if (cached < capacity || cached > 4U*capacity) {
    siqs_tlp_batch_destroy(*tail); *tail = NULL;
    if (siqs_tlp_batch_create(tail, store, capacity) != SIQS_TLP_OK)
      croak("SIQS residual-first: tail scratch allocation");
  }
  return *tail;
}

/* Most screened norms are rejected. Grow result descriptors with accepted
 * output instead of reserving two full-batch arrays in every context. */
static siqs_rf_accepted_t *siqs_rf_accepted(struct siqs_rf_state *s,
                                            int lp, size_t index) {
  siqs_rf_accepted_t **values = lp ? &s->lp_accepted : &s->direct;
  size_t *allocated = lp ? &s->lp_accepted_alloc : &s->direct_alloc;
  size_t limit = lp ? s->pool->lp_capacity : s->pool->fb_capacity;
  if (index >= limit) croak("SIQS residual-first: accepted batch overflow");
  if (index == *allocated) {
    size_t next = *allocated ? 2U * *allocated : 64U;
    if (next > limit) next = limit;
    *values = siqs_realloc(*values, siqs_array_size(next, sizeof(**values)));
    *allocated = next;
  }
  return *values + index;
}

static void siqs_rf_clear_lp_queue(struct siqs_rf_state *s) {
  size_t i;
  for (i = 0; i < s->lp_count; ++i) siqs_rf_packet_clear(s->lp_pending[i]);
  siqs_raw_arena_clear(&s->lp_arena);
  s->lp_count = 0;
}

static void siqs_rf_lp_flush(struct siqs_rf_state *s) {
  siqs_ctx_t *ctx = &s->resolver;
  struct siqs_tlp_state *lp = ctx->tlp;
  size_t i, accepted = 0;
#ifdef SIQS_TIMING
  uint64_t start, cpu;
#endif
  if (s->lp_count == 0) return;
  siqs_tlp_ensure_store(ctx);
#ifdef SIQS_TIMING
  start = siqs_timing_now(); cpu = siqs_timing_cpu_now();
#endif
  if (siqs_tlp_batch_gcd(siqs_rf_batch(s, 1, s->lp_count), lp->inputs, lp->gcds, s->lp_count) != SIQS_TLP_OK)
    croak("SIQS residual-first: LP GCD failed");
#ifdef SIQS_TIMING
  ctx->result->profile.tlp_gcd_us += siqs_timing_elapsed(start);
  cpu = siqs_timing_cpu_now()-cpu;
  ctx->result->profile.tlp_gcd_cpu_us += cpu;
  if (s->drain_lp) ctx->result->profile.rf_lp_forced_cpu_us += cpu;
  else ctx->result->profile.rf_lp_bulk_cpu_us += cpu;
#endif
  ++ctx->tlp_batches;
  for (i = 0; i < s->lp_count; ++i) {
    uint64_t labels[3] = {1,1,1}; uint32_t count;
    if (mpz_cmp_ui(lp->gcds[i], 1) == 0) { ++ctx->tlp_gcd_one; continue; }
    if (mpz_cmp(lp->gcds[i], lp->inputs[i]) == 0) ++ctx->tlp_gcd_whole;
    else ++ctx->tlp_gcd_proper;
#ifdef SIQS_TIMING
    start = siqs_timing_now();
#endif
    if (siqs_tlp_complete(ctx, lp->inputs[i], lp->gcds[i], labels, &count)) {
      siqs_rf_accepted_t *a = siqs_rf_accepted(s, 1, accepted++);
      a->packet = s->lp_pending[i]; a->count = count;
      memcpy(a->labels, labels, sizeof(labels)); ++ctx->tlp_completed;
    } else ++ctx->tlp_rejected;
#ifdef SIQS_TIMING
    ctx->result->profile.tlp_complete_us += siqs_timing_elapsed(start);
#endif
  }
  siqs_rf_recover(s, s->lp_accepted, accepted);
  siqs_rf_clear_lp_queue(s);
}

static int siqs_rf_run_body(void *data) {
  struct siqs_rf_state *s = data;
  siqs_ctx_t *ctx = &s->resolver;
  size_t i, direct = 0;
#ifdef SIQS_TIMING
  uint64_t start, cpu;
#endif
  if (ctx->raw_count != 0) croak("SIQS residual-first: unconsumed output");
  for (i = 0; i < s->flight_count; ++i) {
    mpz_srcptr norm = s->flight[i]->rf_norm->norm;
    if (mpz_cmp_ui(norm, 1) == 0) mpz_set_ui(s->inputs[i], 2);
    else mpz_set(s->inputs[i], norm);
  }
#ifdef SIQS_TIMING
  start = siqs_timing_now(); cpu = siqs_timing_cpu_now();
#endif
  if (siqs_tlp_batch_gcd_fb(siqs_rf_batch(s, 0, s->flight_count), s->inputs, s->gcds, s->flight_count) != SIQS_TLP_OK)
    return SIQS_TLP_BAD_ARGUMENT;
#ifdef SIQS_TIMING
  ctx->result->profile.rf_screen_us += siqs_timing_elapsed(start);
  ctx->result->profile.rf_screen_cpu_us += siqs_timing_cpu_now()-cpu;
  ctx->result->profile.rf_inputs += s->flight_count;
#endif
  for (i = 0; i < s->flight_count; ++i) {
    siqs_raw_relation_t *p = s->flight[i];
    uint64_t labels[3] = {1,1,1}; uint32_t count; int status;
    mpz_ptr rest = ctx->eval.rest;
#ifdef SIQS_TIMING
    start = siqs_timing_now();
#endif
    if (mpz_cmp_ui(p->rf_norm->norm, 1) == 0) mpz_set_ui(s->gcds[i], 1);
    mpz_set(p->rf_norm->distinct, s->gcds[i]);
    mpz_set(rest, p->rf_norm->norm); mpz_set(ctx->eval.q, s->gcds[i]);
    while (mpz_cmp_ui(ctx->eval.q, 1) > 0) {
      mpz_divexact(rest, rest, ctx->eval.q);
      mpz_gcd(ctx->eval.q, rest, s->gcds[i]);
    }
#ifdef SIQS_TIMING
    ctx->result->profile.rf_strip_us += siqs_timing_elapsed(start);
#endif
    status = siqs_tlp_classify(ctx, rest, labels, &count);
    if (status > 0) {
      siqs_rf_accepted_t *a = siqs_rf_accepted(s, 0, direct++);
      a->packet = p; a->count = count;
      memcpy(a->labels, labels, sizeof(labels));
    } else if (status == 0) {
      siqs_factor_t *known = siqs_eval_reserve_factors(ctx, p->nfactors);
      uint32_t j;
      for (j = 0; j < p->nfactors; ++j) {
        known[j].row = siqs_raw_factor_row(p, j);
        known[j].exponent = siqs_raw_factor_exponent(p, j);
      }
      s->lp_pending[s->lp_count] = siqs_rf_packet_new(&s->lp_arena,
          p->y, known, p->nfactors, p->rf_norm->norm, p->rf_norm->distinct);
      mpz_set(ctx->tlp->inputs[s->lp_count++], rest);
      if (s->lp_count == s->pool->lp_capacity) siqs_rf_lp_flush(s);
    }
  }
  siqs_rf_recover(s, s->direct, direct);
  if (s->drain_lp) siqs_rf_lp_flush(s);
  return SIQS_TLP_OK;
}

static unsigned siqs_rf_read_counter(struct siqs_rf_pool *p, const unsigned *counter) {
#if SIQS_RF_ATOMICS
  (void)p;
  return __atomic_load_n(counter, __ATOMIC_RELAXED);
#else
  unsigned value;
  pthread_mutex_lock(&p->counter_mutex);
  value = *counter;
  pthread_mutex_unlock(&p->counter_mutex);
  return value;
#endif
}

/* Count overlapping batch executions. Each job owns its batch state. */
static int siqs_rf_run(void *data) {
  struct siqs_rf_state *s = data;
  struct siqs_rf_pool *p = s->pool;
  int status;
#if SIQS_RF_ATOMICS
  unsigned active = __atomic_add_fetch(&p->active_jobs, 1U, __ATOMIC_RELAXED);
  unsigned peak = __atomic_load_n(&p->peak_active_jobs, __ATOMIC_RELAXED);
  while (active > peak && !__atomic_compare_exchange_n(&p->peak_active_jobs,
      &peak, active, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
#else
  pthread_mutex_lock(&p->counter_mutex);
  ++p->active_jobs;
  if (p->active_jobs > p->peak_active_jobs) p->peak_active_jobs = p->active_jobs;
  pthread_mutex_unlock(&p->counter_mutex);
#endif
#ifdef SIQS_RF_TEST_HOOKS
  if (p->started != NULL) p->started(p->started_data);
#endif
  status = siqs_rf_run_body(data);
#if SIQS_RF_ATOMICS
  __atomic_sub_fetch(&p->active_jobs, 1U, __ATOMIC_RELAXED);
#else
  pthread_mutex_lock(&p->counter_mutex);
  --p->active_jobs;
  pthread_mutex_unlock(&p->counter_mutex);
#endif
  return status;
}

static void siqs_rf_merge_counters(siqs_ctx_t *owner, siqs_ctx_t *ctx) {
#define RF_MERGE(name) owner->name += ctx->name; ctx->name = 0;
  RF_MERGE(split_attempts) RF_MERGE(split_square) RF_MERGE(split_siqs)
  RF_MERGE(split_squfof) RF_MERGE(split_ecm) RF_MERGE(split_fail) RF_MERGE(split_rejected)
  RF_MERGE(tlp_candidates) RF_MERGE(tlp_batches) RF_MERGE(tlp_gcd_one)
  RF_MERGE(tlp_gcd_proper) RF_MERGE(tlp_gcd_whole) RF_MERGE(tlp_completed) RF_MERGE(tlp_rejected)
  RF_MERGE(tlp_r2_attempts) RF_MERGE(tlp_r2_resolved) RF_MERGE(tlp_r2_rejected)
  RF_MERGE(tlp_r2_fallback) RF_MERGE(tlp_gap_rejected)
#undef RF_MERGE
#ifdef SIQS_TIMING
#define RF_PROFILE(name) owner->result->profile.name += ctx->result->profile.name; ctx->result->profile.name = 0;
  SIQS_PROFILE_FIELDS(RF_PROFILE)
#undef RF_PROFILE
#define RF_RESULT(name) owner->result->name += ctx->result->name; ctx->result->name = 0;
  RF_RESULT(cofactor_time) RF_RESULT(cofactor_calls) RF_RESULT(primality_time)
  RF_RESULT(primality_prime) RF_RESULT(primality_composite)
#undef RF_RESULT
#endif
}

static void siqs_rf_finish_slot(siqs_ctx_t *ctx, struct siqs_rf_state *s,
                                  int wait) {
  struct siqs_rf_pool *p = ctx->rf;
  siqs_tlp_gcd_job_result_t result;
  size_t i;
#ifdef SIQS_TIMING
  uint64_t wait_start;
#endif
  if (!s->running || ctx->tlp->flushing) return;
  if (!wait && !siqs_tlp_gcd_job_ready(&s->job)) return;
  /* Suppress recursive journal checkpoints until this output is published. */
  ctx->tlp->flushing = 1;
#ifdef SIQS_TIMING
  wait_start = siqs_timing_now();
#endif
  if (!siqs_tlp_gcd_job_wait(&s->job, &result) || result.status != SIQS_TLP_OK)
    croak("SIQS residual-first: job failed");
  s->running = 0;
#ifdef SIQS_TIMING
  ctx->result->profile.rf_wait_us += siqs_timing_elapsed(wait_start);
#endif
  p->inputs_done += s->flight_count;
  ++p->jobs_done;
  p->recovered += s->resolver.raw_count;
  if (s->flight_count < p->fb_capacity) ++p->short_jobs;
  siqs_rf_merge_counters(ctx, &s->resolver);
#ifdef SIQS_TIMING
  ctx->result->profile.rf_job_us += result.gcd_us;
  ctx->result->profile.rf_job_cpu_us += result.cpu_us;
  ++ctx->result->profile.rf_jobs;
  if (s->on_worker) ++ctx->result->profile.rf_worker_jobs;
  else ++ctx->result->profile.rf_coord_jobs;
  if (s->drain_lp) { ++ctx->result->profile.rf_forced_jobs; ctx->result->profile.rf_forced_cpu_us += result.cpu_us; }
  else { ++ctx->result->profile.rf_bulk_jobs; ctx->result->profile.rf_bulk_cpu_us += result.cpu_us; }
#endif
  for (i = 0; i < s->resolver.raw_count; ++i) {
    siqs_raw_relation_t *r = s->resolver.raw[i];
    if (!ctx->factor_found) {
      uint32_t j; siqs_factor_t *f = siqs_eval_reserve_factors(ctx, r->nfactors);
      siqs_raw_relation_t *copy;
      for (j = 0; j < r->nfactors; ++j) {
        f[j].row = siqs_raw_factor_row(r, j); f[j].exponent = siqs_raw_factor_exponent(r, j);
      }
      copy = siqs_raw_relation_new3(ctx, r->y, f, r->nfactors, r->lp1, r->lp2, r->lp3);
      siqs_accept_raw_relation(ctx, copy);
    }
  }
  while (s->resolver.raw_count != 0)
    siqs_raw_relation_free(&s->resolver, s->resolver.raw[--s->resolver.raw_count]);
  siqs_raw_arena_clear(&s->resolver.raw_arena);
  for (i = 0; i < s->flight_count; ++i) siqs_rf_packet_clear(s->flight[i]);
  siqs_raw_arena_clear(&s->flight_arena); s->flight_count = 0;
  s->published_lp_count = s->lp_count;
  ctx->tlp->flushing = 0;
  if (ctx->journal != NULL) sj_maybe_flush(ctx);
}

static void siqs_rf_finish(siqs_ctx_t *ctx, int wait) {
  struct siqs_rf_pool *p = ctx->rf;
  unsigned i;
  if (p == NULL || ctx->tlp->flushing) return;
  for (i = 0; i < p->job_count; ++i) siqs_rf_finish_slot(ctx, p->slots[i], wait);
}

/* The producer arena and each flight arena are distinct. Swap only after
 * this slot has been consumed; other slots can keep running throughout. */
static void siqs_rf_launch(siqs_ctx_t *ctx, struct siqs_rf_state *s, int drain) {
  struct siqs_rf_pool *p = ctx->rf;
  siqs_raw_relation_t **records; siqs_raw_arena_t arena;
  if (s->running || ctx->tlp->flushing)
    croak("SIQS residual-first: launch into a busy slot");
  if (p->count == 0 && (!drain || s->published_lp_count == 0)) return;
  records = s->flight; s->flight = p->pending; p->pending = records;
  arena = s->flight_arena; s->flight_arena = p->pending_arena; p->pending_arena = arena;
  s->flight_count = p->count; p->count = 0; s->drain_lp = drain;
  s->ticket = ++p->next_ticket;
  if (!siqs_tlp_gcd_job_prepare_callback(&s->job, siqs_rf_run, s))
    croak("SIQS residual-first: job busy");
  s->running = 1;
  s->on_worker = ctx->tlp_submit_gcd != NULL &&
      ctx->tlp_submit_gcd(ctx->tlp_submit_data, &s->job);
  if (!s->on_worker) {
    siqs_tlp_gcd_job_run(&s->job);
    /* A fallback waits for its own slot, never an unrelated running job. */
    siqs_rf_finish_slot(ctx, s, 1);
  }
}

static void siqs_rf_slot_init(siqs_ctx_t *, struct siqs_rf_pool *, unsigned);

static void siqs_rf_submit(siqs_ctx_t *ctx, int drain) {
  struct siqs_rf_pool *p = ctx->rf;
  struct siqs_rf_state *chosen = NULL;
  unsigned i, index = 0;
  if (p->count == 0 || ctx->factor_found) return;
  siqs_rf_finish(ctx, 0);
  if (ctx->factor_found) return;
  /* Rotate idle slots so neither persistent LP queue is starved. */
  for (i = 0; i < p->job_count; ++i) {
    unsigned j = (p->next_job+i) % p->job_count;
    if (!p->slots[j]->running) { chosen = p->slots[j]; index = j; break; }
  }
  if (chosen == NULL && p->count == p->fb_capacity && p->job_count < p->job_limit) {
#ifdef SIQS_TIMING
    uint64_t start = siqs_timing_now();
#endif
    /* Grow only for a full producer batch while every current slot is busy.
     * Drains reuse existing scratch. Each context keeps a
     * stable address while workers hold callbacks into it. */
    index = p->job_count;
    siqs_rf_slot_init(ctx, p, index);
    ++p->job_count; chosen = p->slots[index];
#ifdef SIQS_TIMING
    ctx->result->profile.rf_setup_us += siqs_timing_elapsed(start);
#endif
  }
  if (chosen == NULL) {
    /* All flight buffers are full: wait only for the oldest busy slot. */
    for (i = 0; i < p->job_count; ++i)
      if (chosen == NULL || p->slots[i]->ticket < chosen->ticket) {
        chosen = p->slots[i]; index = i;
      }
    siqs_rf_finish_slot(ctx, chosen, 1);
    if (ctx->factor_found) return;
  }
  p->next_job = (index+1U) % p->job_count;
  siqs_rf_launch(ctx, chosen, drain);
}

static void siqs_rf_queue(siqs_ctx_t *ctx, siqs_raw_relation_t *packet) {
  struct siqs_rf_pool *p = ctx->rf;
  if (p == NULL || packet->rf_norm == NULL || p->count >= p->fb_capacity)
    croak("SIQS residual-first: invalid norm queue");
  p->pending[p->count++] = packet;
  if (p->count == p->fb_capacity) siqs_rf_submit(ctx, 0);
}

static void siqs_rf_poll(siqs_ctx_t *ctx) { siqs_rf_finish(ctx, 0); }

static size_t siqs_rf_pending(const siqs_ctx_t *ctx) {
  const struct siqs_rf_pool *p = ctx->rf;
  size_t count = p->count; unsigned i;
  /* Worker-mutated LP counts are read only after taking job completion. */
  for (i = 0; i < p->job_count; ++i)
    count += p->slots[i]->flight_count+p->slots[i]->published_lp_count;
  return count;
}

static void siqs_rf_discard_pending(struct siqs_rf_pool *p) {
  size_t i;
  unsigned slot;
  for (i = 0; i < p->count; ++i) siqs_rf_packet_clear(p->pending[i]);
  siqs_raw_arena_clear(&p->pending_arena);
  p->count = 0;
  for (slot = 0; slot < p->job_count; ++slot) {
    struct siqs_rf_state *s = p->slots[slot];
    siqs_rf_clear_lp_queue(s);
    s->published_lp_count = 0;
  }
}

static void siqs_rf_flush(siqs_ctx_t *ctx) {
  struct siqs_rf_pool *p = ctx->rf;
  unsigned i;
  if (ctx->tlp->flushing) return;
  siqs_rf_finish(ctx, 1);
  if (!ctx->factor_found && p->count != 0) {
    siqs_rf_submit(ctx, 1);
    siqs_rf_finish(ctx, 1);
  }
  /* A zero-norm job drains each slot's independent LP queue. */
  for (i = 0; i < p->job_count && !ctx->factor_found; ++i)
    if (p->slots[i]->published_lp_count != 0)
      siqs_rf_launch(ctx, p->slots[i], 1);
  siqs_rf_finish(ctx, 1);
  /* Once a factor is found, wait for assigned jobs and release the queues. */
  if (ctx->factor_found) siqs_rf_discard_pending(p);
}

static void siqs_rf_capture(siqs_ctx_t *ctx, const siqs_poly_t *poly,
    int32_t x, const mpz_t norm, int negative,
    const uint32_t *small, const uint32_t *exponents, uint32_t count) {
  uint32_t i, used = 0;
  siqs_factor_t *f = siqs_eval_reserve_factors(ctx, count+poly->q_count+2U);
  mpz_ptr y = ctx->eval.rest;
  siqs_raw_relation_t *packet;
  mpz_mul_si(y, poly->DA, x); mpz_add(y, y, poly->B);
  if (mpz_sgn(norm) == 0) {
    mpz_gcd(ctx->eval.q, y, ctx->n);
    if (mpz_cmp_ui(ctx->eval.q, 1) > 0 && mpz_cmp(ctx->eval.q, ctx->n) < 0 &&
        siqs_insert_divisor(ctx->result, ctx->eval.q)) {
      ctx->factor_found = 1; siqs_bids_note_progress(ctx);
    }
    return;
  }
  mpz_mod(y, y, ctx->n);
  if (negative) { f[used].row = 0; f[used++].exponent = 1; }
  for (i = 0; i < count; ++i) { f[used].row = small[i]+1U; f[used++].exponent = exponents[i]; }
  if (ctx->params.poly_d == 2) { f[used].row = 1; f[used++].exponent = 1; }
  for (i = 0; i < poly->q_count; ++i) { f[used].row = poly->a_index[i]+1U; f[used++].exponent = 1; }
  used = siqs_rf_coalesce(f, used);
  packet = siqs_rf_packet_new(ctx->buffer_relations ? &ctx->raw_arena : &ctx->rf->pending_arena,
                              y, f, used, norm, NULL);
  if (ctx->buffer_relations) siqs_store_raw(ctx, packet);
  else {
    siqs_rf_queue(ctx, packet);
    if (!ctx->factor_found) siqs_rf_poll(ctx);
  }
}

static void siqs_rf_slot_init(siqs_ctx_t *ctx, struct siqs_rf_pool *p,
                                unsigned j) {
  size_t i;
  struct siqs_rf_state *s;
  siqs_ctx_t *r;
  if (j >= p->job_limit || p->slots[j] != NULL)
    croak("SIQS residual-first: invalid new slot");
  s = siqs_calloc(1, sizeof(*s)); p->slots[j] = s; r = &s->resolver;
  s->owner = ctx; s->pool = p;
  s->fb_store = p->fb_store; s->tree = p->tree; s->leaves = p->leaves;
  s->flight = siqs_calloc(p->fb_capacity, sizeof(*s->flight));
  s->lp_pending = siqs_calloc(p->lp_capacity, sizeof(*s->lp_pending));
  s->inputs = siqs_malloc(siqs_array_size(p->fb_capacity, sizeof(*s->inputs)));
  s->gcds = siqs_malloc(siqs_array_size(p->fb_capacity, sizeof(*s->gcds)));
  for (i = 0; i < p->fb_capacity; ++i) { mpz_init(s->inputs[i]); mpz_init(s->gcds[i]); }
  if (siqs_tlp_batch_create(&s->fb_batch, p->fb_store, p->fb_capacity) != SIQS_TLP_OK)
    croak("SIQS residual-first: FB scratch allocation");
  r->original_n = ctx->original_n; r->params = ctx->params;
  r->multiplier = ctx->multiplier; r->largest_fb_prime = ctx->largest_fb_prime;
  r->shared_fb_tables = 1; r->fb = ctx->fb; r->prime = ctx->prime;
  r->tlp_policy = ctx->tlp_policy; r->verbose = ctx->verbose; r->nthreads = 1;
  r->cofactor_rng.state = siqs_mix64(ctx->cofactor_rng.state ^
      UINT64_C(0x7c369a1e408d2bf5) ^ j);
  mpz_init_set(r->n, ctx->n); mpz_init_set(r->kn, ctx->kn);
  mpz_init_set(r->tlp_upper, ctx->tlp_upper);
  mpz_init(r->eval.q); mpz_init(r->eval.y); mpz_init(r->eval.rest);
  siqs_factor_array_init(&s->result, r->n); r->result = &s->result;
  siqs_tlp_init_capacity(r, p->lp_capacity); r->buffer_relations = 1;
  /* The pool owns this immutable store; each resolver owns its batch. */
  r->tlp->store = p->lp_store;
  /* One callback runs at a time per slot. With equal capacities the FB and
   * LP phases share their largest mutable product tree. Otherwise defer the
   * separate LP workspace until this slot actually has an LP batch to split. */
  if (p->fb_capacity == p->lp_capacity) r->tlp->batch = s->fb_batch;
  if (!siqs_tlp_gcd_job_init(&s->job, s->fb_batch,
#ifdef SIQS_TIMING
        ctx->params.bits >= SIQS_TIMING_MIN_BITS
#else
        0
#endif
      )) croak("SIQS residual-first: job synchronization allocation");
}

static size_t siqs_rf_capacity(size_t capacity) {
  if (capacity == 0 || capacity > 1048576U ||
      (capacity & (capacity-1U)) != 0)
    croak("SIQS: unsupported residual batch capacity");
  return capacity;
}

static void siqs_rf_init(siqs_ctx_t *ctx) {
  struct siqs_rf_pool *p;
  size_t i;
  uint32_t begin = ctx->params.sieve_start, end = ctx->params.fb_size;
#ifdef SIQS_TIMING
  uint64_t start;
#endif
  if (!ctx->residual_first) return;
#ifdef SIQS_TIMING
  start = siqs_timing_now();
#endif
  p = siqs_calloc(1, sizeof(*p)); ctx->rf = p;
#if !SIQS_RF_ATOMICS
  if (pthread_mutex_init(&p->counter_mutex, NULL) != 0)
    croak("SIQS residual-first: could not initialize counter mutex");
#endif
  /* This is a directory of pointers, not preallocated resolver scratch. The
   * existing worker count bounds useful concurrency; contexts grow on demand. */
  p->job_limit = ctx->nthreads > 1 ? ctx->nthreads : 1U;
  p->slots = siqs_calloc(p->job_limit, sizeof(*p->slots));
  p->job_count = 1U;
  p->fb_capacity = siqs_rf_capacity(SIQS_RF_FB_BATCH_SIZE);
  p->lp_capacity = siqs_rf_capacity(SIQS_RF_LP_BATCH_SIZE);
  p->pending = siqs_calloc(p->fb_capacity, sizeof(*p->pending));
  p->leaves = 1; while (p->leaves < ((size_t)end-begin+31U)/32U) p->leaves *= 2U;
  p->tree = siqs_malloc(siqs_array_size(2U*p->leaves, sizeof(*p->tree)));
  for (i = 0; i < 2U*p->leaves; ++i) mpz_init_set_ui(p->tree[i], 1);
  for (i = begin; i < end; ++i)
    mpz_mul_ui(p->tree[p->leaves+(i-begin)/32U], p->tree[p->leaves+(i-begin)/32U], ctx->prime[i]);
  for (i = p->leaves; i-- > 1;) mpz_mul(p->tree[i], p->tree[2U*i], p->tree[2U*i+1U]);
  if (siqs_tlp_prime_store_from_product(&p->fb_store, p->tree[1],
        begin < end ? ctx->prime[begin] : 0,
        begin < end ? ctx->prime[end-1U] : 0, end-begin) != SIQS_TLP_OK ||
      siqs_tlp_prime_store_create(&p->lp_store, ctx->largest_fb_prime,
                                  ctx->params.tlp_cutoff) != SIQS_TLP_OK)
    croak("SIQS residual-first: shared product allocation");
  siqs_rf_slot_init(ctx, p, 0);
#ifdef SIQS_TIMING
  ctx->result->profile.rf_setup_us += siqs_timing_elapsed(start);
#endif
}

static void siqs_rf_clear(siqs_ctx_t *ctx) {
  struct siqs_rf_pool *p = ctx->rf;
  size_t i; unsigned j;
  if (p == NULL) return;
  siqs_rf_flush(ctx);
  if (siqs_rf_read_counter(p, &p->active_jobs) != 0)
    croak("SIQS residual-first: executor active during clear");
  ctx->rf = NULL; ctx->residual_first = 0;
  for (j = 0; j < p->job_count; ++j) {
    struct siqs_rf_state *s = p->slots[j];
    if (!siqs_tlp_gcd_job_clear(&s->job))
      croak("SIQS residual-first: live job during clear");
    siqs_tlp_batch_destroy(s->lp_tail); siqs_tlp_batch_destroy(s->fb_tail);
    /* Detach the borrowed store before the generic context destructor.
     * All borrowing contexts die before either pool-owned product is freed. */
    if (s->resolver.tlp->batch != s->fb_batch)
      siqs_tlp_batch_destroy(s->resolver.tlp->batch);
    s->resolver.tlp->batch = NULL; s->resolver.tlp->store = NULL;
    siqs_ctx_clear(&s->resolver);
    free(s->result.primality); gmp_siqs_free(s->result.values, s->result.count);
    for (i = 0; i < p->fb_capacity; ++i) { mpz_clear(s->inputs[i]); mpz_clear(s->gcds[i]); }
    siqs_tlp_batch_destroy(s->fb_batch);
    siqs_raw_arena_clear(&s->flight_arena); siqs_raw_arena_clear(&s->lp_arena);
    free(s->inputs); free(s->gcds); free(s->selected);
    siqs_rf_recovery_clear(s);
    free(s->flight); free(s->lp_pending); free(s->direct); free(s->lp_accepted);
    free(s);
  }
  for (i = 0; i < 2U*p->leaves; ++i) mpz_clear(p->tree[i]);
  siqs_tlp_prime_store_destroy(p->fb_store); siqs_tlp_prime_store_destroy(p->lp_store);
  siqs_raw_arena_clear(&p->pending_arena);
#if !SIQS_RF_ATOMICS
  if (pthread_mutex_destroy(&p->counter_mutex) != 0)
    croak("SIQS residual-first: could not destroy counter mutex");
#endif
  free(p->pending); free(p->tree); free(p->slots); free(p);
}

#endif
