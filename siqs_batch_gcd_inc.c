/* Private interval-prime products and reusable batch-GCD scratch.
 * Copyright (c) 2026 Dana Jacobsen. See LICENSE.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
typedef struct siqs_tlp_prime_store siqs_tlp_prime_store;
typedef struct siqs_tlp_batch siqs_tlp_batch;

enum {
  SIQS_TLP_OK = 0,
  SIQS_TLP_BAD_ARGUMENT = 1,
  SIQS_TLP_ALLOCATION_FAILED = 2,
  SIQS_TLP_PRIME_ITERATOR_FAILED = 3,
  SIQS_TLP_PRODUCT_CHECK_FAILED = 4
};

typedef struct {
  uint32_t lower_exclusive, cutoff_inclusive;
  uint32_t first_prime, last_prime;
  uint64_t prime_count;
  size_t block_count;
  size_t value_limb_bytes; /* Significant limbs, not allocator/RSS accounting. */
} siqs_tlp_prime_stats;

/* Construct all primes in (lower,cutoff] directly into bounded resident blocks.
 * No file I/O, global cache, uint128_t arithmetic, or full-product merge.
 * lower <= cutoff, including an empty interval, is valid. Iteration currently
 * restricts the cutoff to uint32_t; candidate integers have no bit-size cap.
 * On failure *out is NULL. GMP retains its ordinary fatal-OOM behavior.
 */
static int siqs_tlp_prime_store_create(siqs_tlp_prime_store **out,
                               uint32_t lower, uint32_t cutoff);
#if SIQS_RESIDUAL_FIRST
/* Copy a caller-built product of the desired distinct primes into one block.
 * This is useful for the actual SIQS factor base, which is not a full prime
 * interval. Metadata describes those primes; primality/squarefreeness remain
 * the caller's responsibility. An empty set uses product=1 and zero metadata.
 * The caller's product is unchanged and may be released after construction. */
static int siqs_tlp_prime_store_from_product(siqs_tlp_prime_store **out,
    const mpz_t product, uint32_t first, uint32_t last, uint64_t count);
#endif
/* Destroy only after every context borrowing this store has been destroyed. */
static void siqs_tlp_prime_store_destroy(siqs_tlp_prime_store *store);

/* capacity=0 selects SIQS_TLP_BATCH_SIZE. Other capacities must be powers of
 * two <= 1048576. Contexts borrow the immutable store and own all mutable GMP
 * scratch. Distinct contexts may use the same store concurrently. One context
 * must not be used concurrently. No process-wide GMP allocator hooks are set.
 */
static int siqs_tlp_batch_create(siqs_tlp_batch **out,
                         const siqs_tlp_prime_store *store, size_t capacity);
static void siqs_tlp_batch_destroy(siqs_tlp_batch *batch);
#if SIQS_RESIDUAL_FIRST
static size_t siqs_tlp_batch_capacity(const siqs_tlp_batch *batch);
#endif

/* Write gcd(input[i], product of interval primes), preserving input order.
 * All inputs and outputs are caller-initialized mpz_t arrays. Inputs must be
 * positive integers >1. Inputs are copied before any output is written, so
 * input/output arrays may be identical. Outputs may not alias each other.
 * count <= capacity; count=0 is a no-op and permits NULL arrays. An invalid
 * input or count leaves every output unchanged. Scratch is reused across
 * calls, including a shorter final batch; no mpz_init/clear in processing.
 * This layer performs no primality tests or factor completion. Its caller
 * discards GCD one and completes positive GCDs under the selected LP policy.
 */
static int siqs_tlp_batch_gcd(siqs_tlp_batch *batch, const mpz_t *input,
                      mpz_t *output, size_t count);
#if SIQS_RESIDUAL_FIRST
/* FB screening uses smaller forest roots; LP screening keeps its target. */
static int siqs_tlp_batch_gcd_fb(siqs_tlp_batch *batch, const mpz_t *input,
                      mpz_t *output, size_t count);
#endif



struct siqs_tlp_prime_store {
  mpz_t *blocks;
  size_t allocated;
  siqs_tlp_prime_stats stats;
};

struct siqs_tlp_batch {
  const siqs_tlp_prime_store *store;
  size_t capacity;
  unsigned depth;
  mpz_t *product, *remainder;
  mpz_t piece;
};

static void siqs_tlp_batch_word_product(mpz_t out, const uint64_t *words,
                         size_t first, size_t end, mpz_t *scratch,
                         unsigned depth) {
  size_t middle;
  if (end - first <= 16) {
    mpz_set_ui(out, 1);
    while (first < end) {
#if ULONG_MAX >= UINT64_MAX
      mpz_mul_ui(out, out, (unsigned long)words[first]);
#else
      mpz_import(scratch[depth], 1, 1, sizeof(uint64_t), 0, 0, &words[first]);
      mpz_mul(out, out, scratch[depth]);
#endif
      ++first;
    }
    return;
  }
  middle = first + (end - first) / 2;
  siqs_tlp_batch_word_product(out, words, first, middle, scratch, depth + 1);
  siqs_tlp_batch_word_product(scratch[depth], words, middle, end, scratch, depth + 1);
  mpz_mul(out, out, scratch[depth]);
}

static int siqs_tlp_batch_retain_block(siqs_tlp_prime_store *store, mpz_t block) {
  size_t n = store->stats.block_count;
  if (n == store->allocated) {
    size_t capacity = store->allocated ? store->allocated * 2 : 16;
    mpz_t *values;
    if (capacity < store->allocated || capacity > SIZE_MAX / sizeof(mpz_t))
      return SIQS_TLP_ALLOCATION_FAILED;
    values = realloc(store->blocks, capacity * sizeof(mpz_t));
    if (values == NULL)
      return SIQS_TLP_ALLOCATION_FAILED;
    store->blocks = values;
    store->allocated = capacity;
  }
  mpz_init(store->blocks[n]);
  mpz_swap(store->blocks[n], block);
  store->stats.value_limb_bytes += mpz_size(store->blocks[n]) * sizeof(mp_limb_t);
  ++store->stats.block_count;
  return SIQS_TLP_OK;
}

static const unsigned long siqs_tlp_checksum_primes[2] = {1000000007UL, 1000000009UL};

static int siqs_tlp_batch_finish_block(siqs_tlp_prime_store *store, mpz_t block,
                        const uint64_t *words, size_t count,
                        mpz_t *scratch, uint64_t *actual, unsigned *checksum_seen) {
  unsigned i;
  siqs_tlp_batch_word_product(block, words, 0, count, scratch, 0);
  for (i = 0; i < 2; ++i) {
    unsigned long modulus = siqs_tlp_checksum_primes[i];
    unsigned long residue = mpz_fdiv_ui(block, modulus);
    if (residue == 0) {
      unsigned bit = 1U << i;
      if (*checksum_seen & bit)
        return SIQS_TLP_PRODUCT_CHECK_FAILED;
      /* Remove the checksum prime once so it cannot erase other residues. */
      mpz_divexact_ui(scratch[0], block, modulus);
      residue = mpz_fdiv_ui(scratch[0], modulus);
      if (residue == 0)
        return SIQS_TLP_PRODUCT_CHECK_FAILED;
      *checksum_seen |= bit;
    }
    actual[i] = actual[i] * residue % modulus;
  }
  return siqs_tlp_batch_retain_block(store, block);
}

static int siqs_tlp_prime_store_create(siqs_tlp_prime_store **out,
                               uint32_t lower, uint32_t cutoff) {
  siqs_tlp_prime_store *store;
  uint64_t *words = NULL, packed = 1;
  uint64_t expected[2] = {1, 1}, actual[2] = {1, 1};
  size_t used = 0, capacity = SIQS_TLP_PRODUCT_BLOCK_WORDS;
  mpz_t block, scratch[32];
  uint32_t prime;
  unsigned i, checksum_expected = 0, checksum_seen = 0;
  int result = SIQS_TLP_OK;
  PR32_ITER(iter);
  if (out == NULL)
    return SIQS_TLP_BAD_ARGUMENT;
  *out = NULL;
  if (lower > cutoff)
    return SIQS_TLP_BAD_ARGUMENT;
  store = calloc(1, sizeof(*store));
  if (store == NULL) return SIQS_TLP_ALLOCATION_FAILED;
  store->stats.lower_exclusive = lower;
  store->stats.cutoff_inclusive = cutoff;
  for (i = 0; i < 2; ++i)
    if (lower < siqs_tlp_checksum_primes[i] && siqs_tlp_checksum_primes[i] <= cutoff)
      checksum_expected |= 1U << i;
  mpz_init(block);
  for (i = 0; i < 32; ++i)
    mpz_init(scratch[i]);
  pr32_iter_seek(&iter, lower);
  while ((prime = pr32_iter_next(&iter)) != 0 && prime <= cutoff) {
    if (store->stats.prime_count == 0) {
      words = malloc(capacity * sizeof(*words));
      if (words == NULL) {
        result = SIQS_TLP_ALLOCATION_FAILED;
        break;
      }
      store->stats.first_prime = prime;
    }
    store->stats.last_prime = prime;
    ++store->stats.prime_count;
    for (i = 0; i < 2; ++i) {
      unsigned long modulus = siqs_tlp_checksum_primes[i];
      if (prime != modulus)
        expected[i] = expected[i] * prime % modulus;
    }
    if (packed > UINT64_MAX / prime) {
      words[used++] = packed;
      packed = prime;
      if (used == capacity) {
        result = siqs_tlp_batch_finish_block(store, block, words, used, scratch,
                                             actual, &checksum_seen);
        if (result != SIQS_TLP_OK)
          break;
        used = 0;
      }
    } else {
      packed *= prime;
    }
  }
  if (result == SIQS_TLP_OK && pr32_iter_error(&iter))
    result = SIQS_TLP_PRIME_ITERATOR_FAILED;
  pr32_iter_destroy(&iter);
  if (result == SIQS_TLP_OK) {
    if (packed > 1)
      words[used++] = packed;
    if (used != 0)
      result = siqs_tlp_batch_finish_block(store, block, words, used, scratch,
                                           actual, &checksum_seen);
    if (result == SIQS_TLP_OK &&
        (checksum_seen != checksum_expected ||
         actual[0] != expected[0] || actual[1] != expected[1]))
      result = SIQS_TLP_PRODUCT_CHECK_FAILED;
  }
  mpz_clear(block);
  for (i = 0; i < 32; ++i)
    mpz_clear(scratch[i]);
  free(words);
  if (result != SIQS_TLP_OK) {
    siqs_tlp_prime_store_destroy(store);
    return result;
  }
  *out = store;
  return SIQS_TLP_OK;
}

#if SIQS_RESIDUAL_FIRST
static int siqs_tlp_prime_store_from_product(siqs_tlp_prime_store **out,
    const mpz_t product, uint32_t first, uint32_t last, uint64_t count) {
  siqs_tlp_prime_store *store; mpz_t block; int result;
  if (out == NULL) return SIQS_TLP_BAD_ARGUMENT;
  *out = NULL;
  if (mpz_sgn(product) <= 0 ||
      (count == 0 && (mpz_cmp_ui(product, 1) != 0 || first != 0 || last != 0)) ||
      (count != 0 && (first < 2 || last < first || mpz_cmp_ui(product, 1) <= 0)))
    return SIQS_TLP_BAD_ARGUMENT;
  store = calloc(1, sizeof(*store));
  if (store == NULL) return SIQS_TLP_ALLOCATION_FAILED;
  store->stats.lower_exclusive = first ? first-1U : 0;
  store->stats.cutoff_inclusive = last;
  store->stats.first_prime = first; store->stats.last_prime = last;
  store->stats.prime_count = count;
  if (count != 0) {
    mpz_init_set(block, product); result = siqs_tlp_batch_retain_block(store, block); mpz_clear(block);
    if (result != SIQS_TLP_OK) { siqs_tlp_prime_store_destroy(store); return result; }
  }
  *out = store; return SIQS_TLP_OK;
}
#endif

static void siqs_tlp_prime_store_destroy(siqs_tlp_prime_store *store) {
  size_t i;
  if (store == NULL)
    return;
  for (i = 0; i < store->stats.block_count; ++i)
    mpz_clear(store->blocks[i]);
  free(store->blocks);
  free(store);
}

static int siqs_tlp_batch_create(siqs_tlp_batch **out,
                         const siqs_tlp_prime_store *store, size_t capacity) {
  siqs_tlp_batch *batch;
  size_t i, width;
  unsigned depth = 0;
  if (out == NULL)
    return SIQS_TLP_BAD_ARGUMENT;
  *out = NULL;
  if (capacity == 0)
    capacity = SIQS_TLP_BATCH_SIZE;
  if (store == NULL || capacity > 1048576 || (capacity & (capacity - 1)))
    return SIQS_TLP_BAD_ARGUMENT;
  for (width = capacity; width > 1; width /= 2)
    ++depth;
  batch = calloc(1, sizeof(*batch));
  if (batch == NULL)
    return SIQS_TLP_ALLOCATION_FAILED;
  batch->product = malloc(2 * capacity * sizeof(mpz_t));
  batch->remainder = malloc((depth + 1) * sizeof(mpz_t));
  if (batch->product == NULL || batch->remainder == NULL) {
    free(batch->product);
    free(batch->remainder);
    free(batch);
    return SIQS_TLP_ALLOCATION_FAILED;
  }
  batch->store = store;
  batch->capacity = capacity;
  batch->depth = depth;
  for (i = 0; i < 2 * capacity; ++i)
    mpz_init(batch->product[i]);
  for (i = 0; i <= depth; ++i)
    mpz_init(batch->remainder[i]);
  mpz_init(batch->piece);
  *out = batch;
  return SIQS_TLP_OK;
}

static void siqs_tlp_batch_destroy(siqs_tlp_batch *batch) {
  size_t i;
  if (batch == NULL)
    return;
  for (i = 0; i < 2 * batch->capacity; ++i)
    mpz_clear(batch->product[i]);
  for (i = 0; i <= batch->depth; ++i)
    mpz_clear(batch->remainder[i]);
  mpz_clear(batch->piece);
  free(batch->product);
  free(batch->remainder);
  free(batch);
}

#if SIQS_RESIDUAL_FIRST
static size_t siqs_tlp_batch_capacity(const siqs_tlp_batch *batch) {
  return batch != NULL ? batch->capacity : 0;
}
#endif

static void siqs_tlp_batch_descend(siqs_tlp_batch *batch, mpz_t *output, size_t count,
                     size_t node, unsigned depth, size_t first, size_t width) {
  if (node >= batch->capacity) {
    mpz_gcd(output[first], batch->remainder[depth], batch->product[node]);
    return;
  }
  mpz_mod(batch->remainder[depth + 1], batch->remainder[depth],
          batch->product[2 * node]);
  siqs_tlp_batch_descend(batch, output, count, 2 * node, depth + 1, first, width / 2);
  if (first + width / 2 < count) {
    mpz_mod(batch->remainder[depth + 1], batch->remainder[depth],
            batch->product[2 * node + 1]);
    siqs_tlp_batch_descend(batch, output, count, 2 * node + 1, depth + 1,
            first + width / 2, width / 2);
  }
}

static int siqs_tlp_batch_gcd_internal(siqs_tlp_batch *batch, const mpz_t *input,
                      mpz_t *output, size_t count, int smaller_roots) {
  size_t i, j, roots, width, node, first, bits;
  size_t input_bits = 0, store_bits = 0;
  unsigned root_depth = 0;
  if (batch == NULL || count > batch->capacity ||
      (count != 0 && (input == NULL || output == NULL)))
    return SIQS_TLP_BAD_ARGUMENT;
  for (i = 0; i < count; ++i)
    if (mpz_cmp_ui(input[i], 1) <= 0)
      return SIQS_TLP_BAD_ARGUMENT;
  if (count == 0)
    return SIQS_TLP_OK;
  /* Avoid building ancestors whose products already exceed the fixed
   * prime product. This geometry only selects an exact product forest. */
  for (i = 0; i < count; ++i) {
    bits = mpz_sizeinbase(input[i], 2);
    input_bits = bits > SIZE_MAX - input_bits ? SIZE_MAX : input_bits + bits;
  }
  for (i = 0; i < batch->store->stats.block_count; ++i) {
    bits = mpz_sizeinbase(batch->store->blocks[i], 2);
    store_bits = bits > SIZE_MAX - store_bits ? SIZE_MAX : store_bits + bits;
  }
  /* Smaller FB roots trade more root reductions for less product-tree work. */
  if (smaller_roots && store_bits > 1) store_bits /= 2;
  if (store_bits == 0) store_bits = 1;
  roots = 1;
  while (roots < batch->capacity && input_bits / roots > store_bits) {
    roots *= 2; ++root_depth;
  }
  for (i = 0; i < count; ++i)
    mpz_set(batch->product[batch->capacity + i], input[i]);
  for (i = count; i < batch->capacity; ++i)
    mpz_set_ui(batch->product[batch->capacity + i], 1);
  for (i = batch->capacity - 1; i >= roots; --i)
    mpz_mul(batch->product[i], batch->product[2 * i], batch->product[2 * i + 1]);
  width = batch->capacity / roots;
  for (j = 0; j < roots && j * width < count; ++j) {
    node = roots + j; first = j * width;
    mpz_set_ui(batch->remainder[root_depth], 1);
    for (i = 0; i < batch->store->stats.block_count; ++i) {
      mpz_mod(batch->piece, batch->store->blocks[i], batch->product[node]);
      mpz_mul(batch->remainder[root_depth], batch->remainder[root_depth], batch->piece);
      mpz_mod(batch->remainder[root_depth], batch->remainder[root_depth], batch->product[node]);
    }
    siqs_tlp_batch_descend(batch, output, count, node, root_depth, first, width);
  }
  return SIQS_TLP_OK;
}

static int siqs_tlp_batch_gcd(siqs_tlp_batch *batch, const mpz_t *input,
                      mpz_t *output, size_t count) {
  return siqs_tlp_batch_gcd_internal(batch, input, output, count, 0);
}

#if SIQS_RESIDUAL_FIRST
static int siqs_tlp_batch_gcd_fb(siqs_tlp_batch *batch, const mpz_t *input,
                      mpz_t *output, size_t count) {
  return siqs_tlp_batch_gcd_internal(batch, input, output, count, 1);
}
#endif
