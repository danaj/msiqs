/* Canonical job and relation payloads. Included by siqs.c after PSIQS. */
#include "dmsiqs_registry.h"

#if !defined(PSIQS) || !SIQS_RESIDUAL_FIRST
# error "dmsiqs requires the pthread residual-first build"
#endif

#define DS_MPZ_BYTES ((MPU_SIQS_MAX_BITS + 7U) / 8U)
#define DS_RELATION_FACTORS 1024U

#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
# error "dmsiqs requires IEEE binary64 doubles"
#endif

/* Only resolved fields used during collection are transmitted. The host
 * passes its exact score weights and thresholds across architecture changes. */
#define DS_PARAMS_U32(X) \
  X(fb_size) X(half_interval) X(q_count) X(poly_d) X(max_large_primes) \
  X(sieve_start) X(sieve_free_units) X(tlp_cutoff)
#define DS_PARAMS_U8(X) \
  X(sieve_initial) X(stage1_bias) X(sieve_raw_scores) X(sieve_saturating)

static void ds_put_mpz(ds_buf *buf, const mpz_t value) {
  unsigned char bytes[DS_MPZ_BYTES];
  size_t count = 0;
  if (mpz_sgn(value) < 0 || mpz_sizeinbase(value, 2) > MPU_SIQS_MAX_BITS) {
    buf->bad = 1;
    return;
  }
  mpz_export(bytes, &count, 1, 1, 1, 0, value);
  ds_put_u8(buf, (uint8_t)count);
  ds_put_bytes(buf, bytes, count);
}

static int ds_get_mpz(ds_reader *r, mpz_t value) {
  uint8_t count = ds_get_u8(r);
  const unsigned char *bytes = ds_get_bytes(r, count);
  if (r->bad || count > DS_MPZ_BYTES || (count && bytes[0] == 0))
    return 0;
  if (count == 0) mpz_set_ui(value, 0);
  else mpz_import(value, count, 1, 1, 1, 0, bytes);
  return 1;
}

static void ds_put_double(ds_buf *buf, double value) {
  uint64_t word;
  memcpy(&word, &value, sizeof(word));
  ds_put_u64(buf, word);
}

static double ds_get_double(ds_reader *r) {
  uint64_t word = ds_get_u64(r);
  double value;
  memcpy(&value, &word, sizeof(word));
  return value;
}

static int ds_encode_setup(ds_buf *buf, const siqs_ctx_t *ctx) {
  const siqs_parameters_t *p = &ctx->params;
  uint32_t i;
  ds_put_mpz(buf, ctx->n);
  ds_put_u32(buf, (uint32_t)ctx->multiplier);
#define DS_SEND_U32(field) ds_put_u32(buf, p->field);
  DS_PARAMS_U32(DS_SEND_U32)
#undef DS_SEND_U32
#define DS_SEND_U8(field) ds_put_u8(buf, p->field);
  DS_PARAMS_U8(DS_SEND_U8)
#undef DS_SEND_U8
  ds_put_u32(buf, p->sieve_score_bound);
  ds_put_u64(buf, p->large_prime_bound);
  ds_put_u64(buf, p->smooth_bound);
  ds_put_double(buf, p->log_scale);
  for (i = 0; i < p->fb_size; ++i) {
    ds_put_u32(buf, ctx->fb[i].p);
    ds_put_u32(buf, ctx->fb[i].sqrt_kn);
    ds_put_u8(buf, ctx->fb[i].logp);
  }
  return !buf->bad;
}

static void ds_worker_clear(siqs_ctx_t *ctx, siqs_factor_array_t *result) {
  siqs_ctx_clear(ctx);
  gmp_siqs_free(result->values, result->count);
  free(result->primality);
  memset(result, 0, sizeof(*result));
}

static int ds_worker_setup(ds_reader *r, siqs_ctx_t *ctx,
                           siqs_factor_array_t *result, uint32_t threads) {
  siqs_parameters_t *p = &ctx->params;
  uint32_t i, previous = 0, score_bound;
  mpz_inits(ctx->n, ctx->kn, ctx->eval.y, ctx->eval.q, ctx->eval.rest,
            ctx->tlp_upper, NULL);
  if (!ds_get_mpz(r, ctx->n) || !mpz_odd_p(ctx->n)) goto invalid;
  p->bits = (uint32_t)mpz_sizeinbase(ctx->n, 2);
  if (p->bits < DMSIQS_MIN_BITS || p->bits > MPU_SIQS_MAX_BITS) goto invalid;
  ctx->multiplier = ds_get_u32(r);
  if (ctx->multiplier == 0 || ctx->multiplier > 4095U) goto invalid;
  mpz_mul_ui(ctx->kn, ctx->n, ctx->multiplier);
#define DS_RECV_U32(field) p->field = ds_get_u32(r);
  DS_PARAMS_U32(DS_RECV_U32)
#undef DS_RECV_U32
#define DS_RECV_U8(field) p->field = ds_get_u8(r);
  DS_PARAMS_U8(DS_RECV_U8)
#undef DS_RECV_U8
  score_bound = ds_get_u32(r);
  p->large_prime_bound = ds_get_u64(r);
  p->smooth_bound = ds_get_u64(r);
  p->log_scale = ds_get_double(r);
  if (r->bad || p->fb_size > SIQS_PACKED_FACTOR_ROW_MASK ||
      p->fb_size <= p->q_count || p->sieve_start == 0 ||
      p->sieve_start > SIQS_POSTFILTER_MAX_SMALL ||
      p->sieve_start >= p->fb_size || p->q_count < 2 ||
      p->q_count > SIQS_MAX_A_PRIMES ||
      p->poly_d != (mpz_fdiv_ui(ctx->kn, 8) == 1 ? 2U : 1U) ||
      p->half_interval < SIQS_SIEVE_ALIGN ||
      p->half_interval > SIQS_HALF_INTERVAL_MAX ||
      (p->half_interval & (SIQS_SIEVE_ALIGN - 1U)) != 0 ||
      (p->max_large_primes != 2 && p->max_large_primes != 3) ||
      p->large_prime_bound > SIQS_LP_MAX || p->smooth_bound == 0 ||
      p->sieve_free_units >= SIQS_SIEVE_SCORE_TOP ||
      p->sieve_initial >= SIQS_SIEVE_SCORE_TOP ||
      p->sieve_raw_scores > 1 || p->sieve_saturating > 1 ||
      score_bound > UINT16_MAX || !(p->log_scale > 0 && isfinite(p->log_scale)) ||
      r->left != (size_t)p->fb_size * 9U)
    goto invalid;
  p->sieve_score_bound = (uint16_t)score_bound;
  ctx->fb = siqs_calloc(p->fb_size, sizeof(*ctx->fb));
  for (i = 0; i < p->fb_size; ++i) {
    uint32_t prime = ds_get_u32(r), root = ds_get_u32(r);
    uint8_t weight = ds_get_u8(r);
    if (prime <= previous || prime >= UINT32_C(0x80000000) ||
        (i == 0 ? prime != 2 : !(prime & 1U)) || root >= prime ||
        weight >= SIQS_SIEVE_SCORE_TOP ||
        (uint64_t)root * root % prime != mpz_fdiv_ui(ctx->kn, prime))
      goto invalid;
    ctx->fb[i].p = previous = prime;
    ctx->fb[i].sqrt_kn = root;
    ctx->fb[i].logp = weight;
  }
  if (!ds_reader_done(r) || p->large_prime_bound < previous) goto invalid;
  ctx->largest_fb_prime = previous;
  if (p->max_large_primes == 3) {
    mpz_t cutoff;
    int valid;
    /* Check the bounds before calling the normal 3LP setup, which assumes
     * internally generated parameters. */
    mpz_init(cutoff);
    siqs_mpz_set_u64(ctx->tlp_upper, p->large_prime_bound);
    mpz_mul_ui(ctx->tlp_upper, ctx->tlp_upper, previous);
    mpz_mul_ui(ctx->tlp_upper, ctx->tlp_upper, previous);
    mpz_root(cutoff, ctx->tlp_upper, 3);
    mpz_mul_ui(cutoff, cutoff, SIQS_TLP_CUTOFF_NUMERATOR);
    mpz_fdiv_q_ui(cutoff, cutoff, SIQS_TLP_CUTOFF_DENOMINATOR);
    valid = p->tlp_cutoff > previous && mpz_cmp_ui(cutoff, p->tlp_cutoff) == 0;
    mpz_clear(cutoff);
    if (!valid) goto invalid;
    siqs_tlp_set_bounds(ctx);
    ctx->residual_first = 1;
  }
  siqs_factor_array_init(result, ctx->n);
  ctx->result = result;
  ctx->original_n = ctx->n;
  ctx->nthreads = threads;
  ctx->cofactor_rng.state = siqs_mix64(mpz_fdiv_ui(ctx->n, 4294967291UL));
  ctx->distributed_worker = 1;
  siqs_workspace_allocate(ctx);
  if (p->max_large_primes == 3) {
    siqs_tlp_init(ctx);
    siqs_rf_init(ctx);
  }
  return 1;

invalid:
  siqs_ctx_clear(ctx);
  return 0;
}

static int ds_encode_relation(ds_buf *buf, const siqs_raw_relation_t *raw) {
  uint32_t i;
  uint8_t count = raw->lp3 > 1 ? 3U : raw->lp1 > 1 ? 2U : raw->lp2 > 1;
  if (raw->lp3 == 0 || raw->nfactors > DS_RELATION_FACTORS) return 0;
  ds_put_mpz(buf, raw->y);
  ds_put_u8(buf, count);
  if (count >= 2) ds_put_u64(buf, raw->lp1);
  if (count >= 1) ds_put_u64(buf, raw->lp2);
  if (count == 3) ds_put_u64(buf, raw->lp3);
  ds_put_u32(buf, raw->nfactors);
  for (i = 0; i < raw->nfactors; ++i)
    ds_put_u32(buf, siqs_pack_raw_factor(siqs_raw_factor_row(raw, i),
                                        siqs_raw_factor_exponent(raw, i)));
  return !buf->bad;
}

static siqs_raw_relation_t *ds_decode_relation(siqs_ctx_t *ctx, ds_reader *r,
                                               mpz_t y) {
  uint64_t labels[3] = {1, 1, 1}, previous = ctx->largest_fb_prime;
  uint32_t i, count, nfactors;
  siqs_factor_t factors[DS_RELATION_FACTORS];
  if (!ds_get_mpz(r, y) || mpz_cmp(y, ctx->n) >= 0) return NULL;
  count = ds_get_u8(r);
  if (count > ctx->params.max_large_primes) return NULL;
  for (i = 0; i < count; ++i) {
    uint64_t label = ds_get_u64(r);
    if (label < previous || label <= ctx->largest_fb_prime ||
        label > ctx->params.large_prime_bound)
      return NULL;
    labels[count == 1 ? 1 : i] = previous = label;
  }
  nfactors = ds_get_u32(r);
  if (r->bad || nfactors > DS_RELATION_FACTORS || nfactors > r->left / 4U)
    return NULL;
  for (i = 0; i < nfactors; ++i) {
    uint32_t packed = ds_get_u32(r);
    uint32_t row = packed & SIQS_PACKED_FACTOR_ROW_MASK;
    uint32_t exponent = packed >> SIQS_PACKED_FACTOR_ROW_BITS;
    if (row > ctx->params.fb_size || exponent == 0 ||
        (i != 0 && row <= factors[i-1].row)) return NULL;
    factors[i].row = row;
    factors[i].exponent = exponent;
  }
  return siqs_raw_relation_new3(ctx, y, factors, nfactors,
                               labels[0], labels[1], labels[2]);
}

static int ds_import_relations(siqs_ctx_t *ctx, ds_reader *r, uint32_t *records) {
  uint32_t i, count = ds_get_u32(r);
  ds_reader admission = *r;
  mpz_t y;
  int valid = 0;
  *records = 0;
  if (r->bad || count == 0 || count > r->left / 6U) return 0;
  mpz_init(y);
  /* Validate the entire frame before changing the collector. Packed partials
   * use a stack-like arena, so release each one while it is still the tail. */
  for (i = 0; i < count; ++i) {
    siqs_raw_relation_t *raw = ds_decode_relation(ctx, r, y);
    int congruent;
    if (raw == NULL) goto done;
    congruent = siqs_raw_relation_congruent(ctx, raw);
    siqs_raw_relation_free(ctx, raw);
    if (!congruent) goto done;
  }
  if (!ds_reader_done(r)) goto done;
  /* Replay the immutable bytes in wire order. Admission can now retain the
   * record or reclaim it immediately after archiving or deduplication. */
  for (i = 0; i < count && !ctx->factor_found; ++i) {
    siqs_raw_relation_t *raw = ds_decode_relation(ctx, &admission, y);
    if (raw == NULL) croak("DMSIQS: failed to decode a validated relation");
    siqs_accept_raw_relation(ctx, raw);
  }
  *records = count;
  valid = 1;
done:
  mpz_clear(y);
  return valid;
}

static int ds_import_divisor(siqs_ctx_t *ctx, ds_reader *r) {
  mpz_t value;
  int valid;
  mpz_init(value);
  valid = ds_get_mpz(r, value) && ds_reader_done(r) &&
          mpz_cmp_ui(value, 1) > 0 && mpz_cmp(value, ctx->n) < 0 &&
          mpz_divisible_p(ctx->n, value);
  if (valid && siqs_insert_divisor(ctx->result, value)) ctx->factor_found = 1;
  mpz_clear(value);
  return valid;
}

static int ds_choose_family(siqs_ctx_t *ctx, siqs_poly_t *poly) {
  return ctx->journal != NULL ? siqs_choose_A(ctx, poly) : siqs_choose_A_sar(ctx, poly);
}

#include "dmsiqs_worker.inc.c"
#include "dmsiqs_host.inc.c"

mpz_t *dmsiqs_factor(ds_host *host, const mpz_t n, uint32_t *nfactors,
                     uint32_t trial_start, int verbose, uint32_t threads,
                     uint32_t flags, siqs_journal_t *journal) {
  return siqs_factor_with_journal(n, nfactors, trial_start, verbose,
                                 threads, journal, flags, host);
}

void dmsiqs_close(ds_host *host, siqs_journal_t *journal, int verbose) {
  ds_host_close(host, journal, verbose);
}

int dmsiqs_worker(const char *address, const char *port, uint32_t threads,
                  const char *name, const char *coordinator, const char *pool,
                  const char *advertise, int verbose) {
  return ds_worker_server(address, port, threads, name, coordinator, pool, advertise, verbose);
}
