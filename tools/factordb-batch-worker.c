/* Persistent, serial factoring worker linked with MPU-GMP in standalone mode.
 * A bare decimal uses the startup mode. A tagged request runs one stage:
 * factor N | trial N | pm1 N B1 B2 | ecm N B1 B2 curves seed | siqs N
 * Separate fields with tabs. Each reply is ok, n, a space-separated
 * multiplicative partition, and one p/c marker per part, separated by tabs.
 * An invalid request produces an error reply on the same stream.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ecm.h"
#include "factor.h"
#include "gmp_main.h"
#include "siqs.h"

#define TRIAL_BOUND 64000UL
#define MAX_INPUT_BITS 8192U
#define MAX_ECM_STAGES 64U
#define MAX_REQUEST 4096U

enum method { FACTOR, TRIAL, PM1, ECM, SIQS };
enum mode { AUTO, LIGHT, ALL, STOCK };

typedef struct {
  enum method method;
  UV b1, b2, curves;
  uint64_t seed;
} stage_t;

typedef struct {
  mpz_t value;
  unsigned long exponent;
} piece_t;

typedef struct {
  piece_t *pieces;
  size_t count, capacity;
} partition_t;

/* Keep the direct worker defaults in sync with the coordinator. */
static const stage_t baseline[] = {
  {ECM,  200,  10000, 4, 0},
  {ECM,  600,  30000, 8, 0},
  {ECM, 2000, 100000, 2, 0}
};

static int decimal(const char *s)
{
  if (*s == '\0') return 0;
  for (; *s != '\0'; s++)
    if (*s < '0' || *s > '9') return 0;
  return 1;
}

static int parse_u64(const char *s, uint64_t *out)
{
  unsigned long long value;
  if (!decimal(s)) return 0;
  errno = 0;
  value = strtoull(s, NULL, 10);
  if (errno != 0 || value > UINT64_MAX) return 0;
  *out = (uint64_t)value;
  return 1;
}

static int parse_bound(const char *s, UV *out)
{
  uint64_t value;
  if (!parse_u64(s, &value) || value > UV_MAX) return 0;
  *out = (UV)value;
  return 1;
}

/* Return zero if a request has more fields than are allowed. */
static size_t fields(char *line, char sep, char **out, size_t capacity)
{
  size_t count = 0;
  for (;;) {
    if (count == capacity) return 0;
    out[count++] = line;
    line = strchr(line, sep);
    if (line == NULL) return count;
    *line++ = '\0';
  }
}

static int parse_ecm(stage_t *stage, char **args)
{
  return parse_bound(args[0], &stage->b1) && stage->b1 >= 2 &&
         parse_bound(args[1], &stage->b2) && stage->b2 >= stage->b1 &&
         parse_bound(args[2], &stage->curves) && stage->curves > 0;
}

static int parse_stage(stage_t *stage, char **args, size_t count)
{
  if (count == 2) {
    if (strcmp(args[0], "factor") == 0) stage->method = FACTOR;
    else if (strcmp(args[0], "trial") == 0) stage->method = TRIAL;
    else if (strcmp(args[0], "siqs") == 0) stage->method = SIQS;
    else return 0;
    return 1;
  }
  if (count == 4 && strcmp(args[0], "pm1") == 0) {
    stage->method = PM1;
    return parse_bound(args[2], &stage->b1) && stage->b1 >= 7 &&
           parse_bound(args[3], &stage->b2) && stage->b2 >= stage->b1;
  }
  if (count == 6 && strcmp(args[0], "ecm") == 0) {
    stage->method = ECM;
    return parse_ecm(stage, args + 2) && parse_u64(args[5], &stage->seed);
  }
  return 0;
}

static void partition_append(partition_t *part, mpz_srcptr value,
                             unsigned long exponent)
{
  size_t i;
  for (i = 0; i < part->count; i++) {
    if (mpz_cmp(value, part->pieces[i].value) == 0) {
      part->pieces[i].exponent += exponent;
      return;
    }
  }
  if (part->count == part->capacity) {
    size_t capacity = part->capacity ? 2 * part->capacity : 8;
    piece_t *pieces = realloc(part->pieces, capacity * sizeof(*pieces));
    if (pieces == NULL) croak("FactorDB worker: allocation failed");
    part->pieces = pieces;
    part->capacity = capacity;
  }
  mpz_init_set(part->pieces[i].value, value);
  part->pieces[i].exponent = exponent;
  part->count++;
}

static void partition_remove(partition_t *part, size_t index)
{
  mpz_clear(part->pieces[index].value);
  part->count--;
  if (index != part->count) part->pieces[index] = part->pieces[part->count];
}

static void partition_free(partition_t *part)
{
  while (part->count > 0) partition_remove(part, part->count - 1);
  free(part->pieces);
}

static int likely_prime(mpz_srcptr value)
{
  return mpz_probab_prime_p(value, 15) != 0;
}

static int append_split(partition_t *part, mpz_srcptr value, mpz_srcptr divisor)
{
  mpz_t quotient;
  if (mpz_cmp_ui(divisor, 1) <= 0 || mpz_cmp(divisor, value) >= 0 ||
      !mpz_divisible_p(value, divisor))
    return 0;
  mpz_init(quotient);
  mpz_divexact(quotient, value, divisor);
  partition_append(part, divisor, 1);
  partition_append(part, quotient, 1);
  mpz_clear(quotient);
  return 1;
}

static int trial_stage(partition_t *part, mpz_srcptr n)
{
  unsigned long small, power;
  uint32_t exponent;
  mpz_t remaining, divisor;
  void *iter = trial_factor_iterator_create(n, TRIAL_BOUND);
  int ok = 1;
  mpz_inits(remaining, divisor, NULL);
  while (trial_factor_iterator_next(&small, &exponent, iter)) {
    mpz_set_ui(divisor, small);
    partition_append(part, divisor, exponent);
  }
  trial_factor_iterator_n(remaining, iter);
  trial_factor_iterator_destroy(iter);
  if (mpz_cmp_ui(remaining, 1) > 0) {
    power = mpz_perfect_power_p(remaining) ? power_factor(remaining, divisor) : 0;
    if (power > 1)
      partition_append(part, divisor, power);
    else if (mpz_sizeinbase(remaining, 2) <= 64 && !likely_prime(remaining) &&
             _GMP_tinyecm64_factor(remaining, divisor, 0))
      ok = append_split(part, remaining, divisor);
    else
      partition_append(part, remaining, 1);
  }
  mpz_clears(remaining, divisor, NULL);
  return ok;
}

static int run_stage(partition_t *part, mpz_srcptr n, const stage_t *stage)
{
  mpz_t divisor;
  int hit, ok = 1;
  if (stage->method == FACTOR) {
    mpz_t *values;
    int *powers;
    int i, count = factor(n, &values, &powers);
    for (i = 0; i < count; i++) {
      if (powers[i] <= 0) {
        ok = 0;
        break;
      }
      partition_append(part, values[i], (unsigned long)powers[i]);
    }
    clear_factors(count, &values, &powers);
  } else if (stage->method == TRIAL) {
    ok = trial_stage(part, n);
  } else if (likely_prime(n)) {
    partition_append(part, n, 1);
  } else if (stage->method == SIQS) {
    size_t bits = mpz_sizeinbase(n, 2);
    if (bits >= MPU_SIQS_MIN_BITS && bits <= MPU_SIQS_MAX_BITS) {
      uint32_t count, i;
      mpz_t *values = gmp_siqs(n, &count, 0, 0);
      for (i = 0; i < count; i++) partition_append(part, values[i], 1);
      gmp_siqs_free(values, count);
    } else {
      partition_append(part, n, 1);
    }
  } else {
    mpz_init(divisor);
    if (stage->method == PM1)
      hit = _GMP_pminus1_factor(n, divisor, stage->b1, stage->b2);
    else
      hit = _GMP_ecm_factor_projective(n, divisor, stage->b1, stage->b2,
                                       stage->curves, stage->seed, 0);
    if (hit)
      ok = append_split(part, n, divisor);
    else
      partition_append(part, n, 1);
    mpz_clear(divisor);
  }
  return ok && part->count > 0;
}

/* Try a stage on each composite. A successful split can expose more
 * factors at the same bounds. Retrying a miss uses the same seed. */
static int apply_stage(partition_t *part, stage_t stage, uint64_t tranche)
{
  size_t i = 0;
  while (i < part->count) {
    partition_t split = {NULL, 0, 0};
    mpz_srcptr n = part->pieces[i].value;
    unsigned long exponent = part->pieces[i].exponent;
    size_t j;
    if (likely_prime(n)) {
      i++;
      continue;
    }
    if (stage.method == ECM)
      stage.seed = mpz_fdiv_ui(n, 4294967291UL) ^ (tranche * UINT64_C(0x9e3779b97f4a7c15));
    if (!run_stage(&split, n, &stage)) {
      partition_free(&split);
      return 0;
    }
    if (split.count > 1 || split.pieces[0].exponent > 1) {
      partition_remove(part, i);
      for (j = 0; j < split.count; j++)
        partition_append(part, split.pieces[j].value, exponent * split.pieces[j].exponent);
    } else {
      i++;
    }
    partition_free(&split);
  }
  return 1;
}

static int run_direct(partition_t *part, mpz_srcptr n, enum mode mode,
                      mpz_srcptr cutoff, const stage_t *ecm, size_t ecm_count)
{
  static const stage_t stock = {FACTOR, 0, 0, 0, 0};
  static const stage_t trial = {TRIAL, 0, 0, 0, 0};
  static const stage_t pm1 = {PM1, 10000, 100000, 0, 0};
  static const stage_t siqs = {SIQS, 0, 0, 0, 0};
  size_t i;
  if (mode == STOCK || ((mode == AUTO || mode == ALL) && mpz_cmp(n, cutoff) < 0))
    return run_stage(part, n, &stock);
  if (!run_stage(part, n, &trial) || !apply_stage(part, pm1, 0)) return 0;
  for (i = 0; i < ecm_count; i++)
    if (!apply_stage(part, ecm[i], i + 1)) return 0;
  return mode != ALL || apply_stage(part, siqs, 0);
}

static int compare_pieces(const void *a, const void *b)
{
  return mpz_cmp(((const piece_t *)a)->value, ((const piece_t *)b)->value);
}

static int emit_partition(FILE *out, mpz_srcptr n, partition_t *part)
{
  size_t i;
  unsigned long j;
  mpz_t product, power;
  int ok = 1;
  mpz_init_set_ui(product, 1);
  mpz_init(power);
  for (i = 0; i < part->count; i++) {
    const piece_t *piece = &part->pieces[i];
    if (mpz_cmp_ui(piece->value, 1) <= 0 || piece->exponent == 0 ||
        piece->exponent > MAX_INPUT_BITS) {
      ok = 0;
      break;
    }
    mpz_pow_ui(power, piece->value, piece->exponent);
    mpz_mul(product, product, power);
    if (mpz_cmp(product, n) > 0) {
      ok = 0;
      break;
    }
  }
  ok = ok && mpz_cmp(product, n) == 0;
  mpz_clears(product, power, NULL);
  if (!ok) return 0;
  qsort(part->pieces, part->count, sizeof(*part->pieces), compare_pieces);
  gmp_fprintf(out, "ok\t%Zd\t", n);
  for (i = 0; i < part->count; i++)
    for (j = 0; j < part->pieces[i].exponent; j++)
      gmp_fprintf(out, "%s%Zd", (i == 0 && j == 0) ? "" : " ", part->pieces[i].value);
  fputc('\t', out);
  for (i = 0; i < part->count; i++) {
    char marker = likely_prime(part->pieces[i].value) ? 'p' : 'c';
    for (j = 0; j < part->pieces[i].exponent; j++) fputc(marker, out);
  }
  fputc('\n', out);
  return 1;
}

/* Preserve line boundaries after an oversized or malformed request. */
static int read_request(char *line, size_t capacity)
{
  size_t used = 0;
  int c, invalid = 0;
  while ((c = fgetc(stdin)) != EOF && c != '\n') {
    if (used >= capacity - 1 || c == '\0')
      invalid = 1;
    else
      line[used++] = (char)c;
  }
  if (invalid) return -1;
  if (c == EOF && used == 0) return 0;
  if (used > 0 && line[used - 1] == '\r') used--;
  line[used] = '\0';
  return 1;
}

static void usage(FILE *out)
{
  fputs("Usage: factordb-batch-worker [--mode auto|light|all|factor]\n"
        "                             [--factor-through DIGITS]\n"
        "                             [--ecm B1:B2:CURVES ...]\n"
        "Read one decimal integer per line. The default is auto with a\n"
        "50-digit inclusive factor() cutoff. Repeated --ecm options replace\n"
        "the baseline ECM ladder. Output is a tab-separated partition reply.\n", out);
}

int main(int argc, char **argv)
{
  stage_t overrides[MAX_ECM_STAGES];
  const stage_t *ecm = baseline;
  size_t override_count = 0, ecm_count = sizeof(baseline) / sizeof(*baseline);
  enum mode mode = AUTO;
  UV cutoff = 50;
  mpz_t n, cutoff_value;
  int i, fd, status = 0;
  FILE *out;
  char line[MAX_REQUEST];

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      const char *name = argv[++i];
      if (strcmp(name, "auto") == 0) mode = AUTO;
      else if (strcmp(name, "light") == 0) mode = LIGHT;
      else if (strcmp(name, "all") == 0) mode = ALL;
      else if (strcmp(name, "factor") == 0) mode = STOCK;
      else goto bad_option;
    } else if (strcmp(argv[i], "--factor-through") == 0 && i + 1 < argc) {
      if (!parse_bound(argv[++i], &cutoff) || cutoff > MAX_INPUT_BITS)
        goto bad_option;
    } else if (strcmp(argv[i], "--ecm") == 0 && i + 1 < argc) {
      char *args[3];
      if (override_count == MAX_ECM_STAGES ||
          fields(argv[++i], ':', args, 3) != 3 ||
          !parse_ecm(&overrides[override_count], args))
        goto bad_option;
      overrides[override_count].method = ECM;
      overrides[override_count++].seed = 0;
    } else {
      goto bad_option;
    }
  }
  if (override_count > 0) {
    ecm = overrides;
    ecm_count = override_count;
  }

  /* The library can print a diagnostic to stdout before a fatal error.
   * Keep the protocol on its own descriptor. */
  fd = dup(STDOUT_FILENO);
  if (fd < 0 || (out = fdopen(fd, "w")) == NULL ||
      dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    perror("factordb-batch-worker: output");
    return 1;
  }
  _GMP_init();
  mpz_inits(n, cutoff_value, NULL);
  mpz_ui_pow_ui(cutoff_value, 10, cutoff);
  while ((i = read_request(line, sizeof(line))) != 0) {
    stage_t stage = {FACTOR, 0, 0, 0, 0};
    partition_t part = {NULL, 0, 0};
    char *args[6], *integer;
    size_t count = i < 0 ? 0 : fields(line, '\t', args, 6);
    if (count == 0 || (count > 1 && !parse_stage(&stage, args, count))) {
      fputs("error\tinvalid request\n", out);
    } else {
      int ok;
      integer = args[count > 1 ? 1 : 0];
      if (!decimal(integer) || mpz_set_str(n, integer, 10) != 0 ||
          mpz_cmp_ui(n, 2) < 0 || mpz_sizeinbase(n, 2) > MAX_INPUT_BITS) {
        fputs("error\texpected decimal integer >= 2 of at most 8192 bits\n", out);
      } else {
        ok = count == 1 ? run_direct(&part, n, mode, cutoff_value, ecm, ecm_count)
                        : run_stage(&part, n, &stage);
        if (!ok || !emit_partition(out, n, &part))
          fputs("error\tinvalid factor partition from MPU-GMP\n", out);
      }
    }
    partition_free(&part);
    if (fflush(out) == EOF) {
      status = 1;
      break;
    }
  }
  mpz_clears(n, cutoff_value, NULL);
  _GMP_destroy();
  if (ferror(stdin)) status = 1;
  if (fclose(out) == EOF) status = 1;
  return status;

bad_option:
  usage(stderr);
  return 2;
}
