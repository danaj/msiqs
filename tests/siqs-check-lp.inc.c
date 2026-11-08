/* Independent finite-space checks for the shared LP combiner. Included by
 * siqs-lp-check.c after siqs.c; these synthetic vectors need no modular square roots. */

static uint64_t lpc_test_random(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  return *state = x;
}

static uint32_t lpc_test_parity(const uint32_t *original, uint32_t count,
                              const uint64_t *selection) {
  uint32_t i, parity = 0;
  for (i = 0; i < count; ++i)
    if ((selection[i / 64U] >> (i % 64U)) & 1U) parity ^= original[i];
  return parity;
}

/* Forward evaluation deliberately differs from the implementation's reverse
 * lane propagation. Every node is evaluated as an explicit set of originals. */
static uint64_t *lpc_test_nodes(const siqs_lp_combiner_t *s, uint32_t words) {
  uint64_t *values = siqs_calloc((size_t)s->node_count * words, sizeof(*values));
  uint32_t i, w;
  for (i = 0; i < s->node_count; ++i) {
    const siqs_lp_node_t *node = s->nodes + i;
    uint64_t *value = values + (size_t)i * words;
    if (node->right == UINT32_MAX) {
      assert(node->left < s->accepted);
      value[node->left / 64U] = UINT64_C(1) << (node->left % 64U);
    } else {
      assert(node->left < i && node->right < i);
      for (w = 0; w < words; ++w)
        value[w] = values[(size_t)node->left * words + w]
                 ^ values[(size_t)node->right * words + w];
    }
  }
  return values;
}

/* Rank of explicit original-coordinate vectors, independent of the LP rows
 * and of the collector's choice or replacement of pivots. */
static uint32_t lpc_test_rank(const uint64_t *vectors, uint32_t count,
                            uint32_t coordinates, uint32_t words) {
  uint64_t *pivots = siqs_calloc((size_t)coordinates * words, sizeof(*pivots));
  uint64_t *work = siqs_malloc((size_t)words * sizeof(*work));
  uint8_t *occupied = siqs_calloc(coordinates, 1);
  uint32_t i, rank = 0;
  for (i = 0; i < count; ++i) {
    uint32_t p, w;
    memcpy(work, vectors + (size_t)i * words, (size_t)words * sizeof(*work));
    for (p = coordinates; p != 0; --p) {
      uint32_t bit = p - 1U;
      if (!((work[bit / 64U] >> (bit % 64U)) & 1U)) continue;
      if (!occupied[bit]) {
        memcpy(pivots + (size_t)bit * words, work, (size_t)words * sizeof(*work));
        occupied[bit] = 1; ++rank;
        break;
      }
      for (w = 0; w < words; ++w) work[w] ^= pivots[(size_t)bit * words + w];
    }
  }
  free(occupied); free(work); free(pivots);
  return rank;
}

static void lpc_test_context(siqs_ctx_t *ctx, uint32_t first_lp,
                             uint32_t lp_rows, uint32_t capacity) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->params.fb_size = first_lp - 1U;
  ctx->full = siqs_calloc(capacity, sizeof(*ctx->full));
  ctx->full_alloc = capacity;
  siqs_lp_combiner_init(ctx);
  ctx->lp_combiner->label_count = lp_rows;
  ctx->lp_combiner->label_alloc = lp_rows;
  ctx->lp_combiner->basis = siqs_calloc(lp_rows, sizeof(*ctx->lp_combiner->basis));
}

static void lpc_test_append(siqs_ctx_t *ctx, uint32_t parity, uint32_t serial) {
  siqs_full_relation_t *relation = siqs_calloc(1, sizeof(*relation));
  uint32_t first_lp = ctx->params.fb_size + 1U;
  uint32_t rows = first_lp + ctx->lp_combiner->label_count, row;
  mpz_init_set_ui(relation->y, 1);
  relation->factors = siqs_calloc(rows, sizeof(*relation->factors));
  for (row = 0; row < rows; ++row) {
    uint32_t odd = (parity >> row) & 1U;
    /* Keep even FB factors to catch accidental presence-vs-parity handling.
     * An LP-free vector sometimes contains one LP square as well. */
    if (row < first_lp || odd ||
        (row == first_lp && (parity >> first_lp) == 0 && (serial & 1U))) {
      siqs_factor_t *factor = relation->factors + relation->nfactors++;
      factor->row = row;
      factor->exponent = row < first_lp ? 2U + odd : (odd ? 1U : 2U);
    }
  }
  assert(ctx->full_count < ctx->full_alloc);
  ctx->full[ctx->full_count++] = relation;
  siqs_lp_combiner_add(ctx);
}

static void lpc_test_release(siqs_ctx_t *ctx) {
  uint32_t i;
  for (i = 0; i < ctx->full_count; ++i) siqs_full_relation_free(ctx->full[i]);
  free(ctx->full);
  siqs_lp_combiner_clear(ctx);
}

/* Check every prefix, so a cycle must retain its meaning after future basis
 * swaps. Exhaustive mode enumerates both the full LP kernel and its image. */
static void lpc_test_prefix(siqs_ctx_t *ctx, const uint32_t *original,
                            int exhaustive, uint64_t *random_state) {
  const siqs_lp_combiner_t *s = ctx->lp_combiner;
  uint32_t first_lp = ctx->params.fb_size + 1U;
  uint32_t fb_mask = (UINT32_C(1) << first_lp) - 1U;
  uint32_t words = (s->accepted + 63U) / 64U, i, j, used = 0, rank = 0;
  uint64_t basis_entries = 0;
  uint64_t *nodes = lpc_test_nodes(s, words);
  uint64_t *spanning = siqs_calloc((size_t)s->accepted * words, sizeof(*spanning));
  unsigned long nrows, ncols;
  la_col_t *matrix;
  assert(s->accepted == ctx->full_count && s->rank + s->cycle_count == s->accepted);
  for (i = 0; i < s->label_count; ++i) {
    const siqs_lp_basis_t *basis = s->basis + i;
    uint32_t parity = 0;
    const uint64_t *vector;
    if (basis->weight == 0) continue;
    assert(basis->node < s->node_count);
    assert(basis->data[basis->weight - 1U] == first_lp + i);
    for (j = 0; j < basis->weight; ++j) {
      assert(basis->data[j] >= first_lp && basis->data[j] < first_lp + s->label_count);
      assert(j == 0 || basis->data[j-1U] < basis->data[j]);
      parity ^= UINT32_C(1) << basis->data[j];
    }
    vector = nodes + (size_t)basis->node * words;
    assert((lpc_test_parity(original, s->accepted, vector) & ~fb_mask) == parity);
    memcpy(spanning + (size_t)used++ * words, vector, (size_t)words * sizeof(*vector));
    basis_entries += basis->weight; ++rank;
  }
  assert(rank == s->rank && basis_entries == s->basis_entries);
  for (i = 0; i < s->cycle_count; ++i) {
    const uint64_t *vector;
    assert(s->cycles[i] < s->node_count);
    vector = nodes + (size_t)s->cycles[i] * words;
    assert((lpc_test_parity(original, s->accepted, vector) & ~fb_mask) == 0);
    memcpy(spanning + (size_t)used++ * words, vector, (size_t)words * sizeof(*vector));
  }
  assert(used == s->accepted);
  assert(lpc_test_rank(spanning, used, s->accepted, words) == s->accepted);
  free(spanning);

  matrix = siqs_lp_combiner_matrix(ctx, &nrows, &ncols);
  assert(nrows == first_lp && ncols == s->cycle_count);
  for (i = 0; i < s->cycle_count; ++i) {
    uint32_t parity = 0;
    assert(matrix[i].orig == s->cycles[i]);
    for (j = 0; j < matrix[i].weight; ++j) {
      assert(matrix[i].data[j] < first_lp);
      assert(j == 0 || matrix[i].data[j-1U] < matrix[i].data[j]);
      parity ^= UINT32_C(1) << matrix[i].data[j];
    }
    assert(parity == lpc_test_parity(original, s->accepted,
                                   nodes + (size_t)matrix[i].orig * words));
  }

  if (exhaustive) {
    uint32_t selection, images = UINT32_C(1) << s->accepted;
    uint32_t combinations = UINT32_C(1) << s->cycle_count, before = 0, after = 0;
    uint8_t *seen = siqs_calloc(images, 1);
    assert(s->accepted <= 16U && words == 1);
    for (selection = 0; selection < combinations; ++selection) {
      uint64_t image = 0;
      uint32_t reduced_parity = 0;
      for (i = 0; i < s->cycle_count; ++i) if ((selection >> i) & 1U) {
        image ^= nodes[s->cycles[i]];
        for (j = 0; j < matrix[i].weight; ++j)
          reduced_parity ^= UINT32_C(1) << matrix[i].data[j];
      }
      assert(image < images && !seen[image]);
      seen[image] = 1;
      assert(lpc_test_parity(original, s->accepted, &image) == reduced_parity);
      if (reduced_parity == 0) ++after;
    }
    for (selection = 0; selection < images; ++selection) {
      uint64_t original_selection = selection;
      uint32_t parity = lpc_test_parity(original, s->accepted, &original_selection);
      assert((seen[selection] != 0) == ((parity & ~fb_mask) == 0));
      if (parity == 0) ++before;
    }
    assert(before == after);
    free(seen);
  }

  if (s->cycle_count != 0) {
    uint32_t columns = s->cycle_count + 3U;
    la_col_t *permuted = siqs_calloc(columns, sizeof(*permuted));
    uint64_t *lanes = siqs_malloc((size_t)columns * sizeof(*lanes));
    uint64_t *expected = siqs_calloc(s->accepted, sizeof(*expected)), *expanded;
    for (i = 0; i < columns; ++i) {
      const uint64_t *vector;
      uint32_t cycle = i < s->cycle_count ? s->cycle_count - 1U - i
                                        : (i - s->cycle_count) % s->cycle_count;
      permuted[i].orig = s->cycles[cycle];
      lanes[i] = i == 0 ? UINT64_MAX : lpc_test_random(random_state);
      vector = nodes + (size_t)permuted[i].orig * words;
      for (j = 0; j < s->accepted; ++j)
        if ((vector[j / 64U] >> (j % 64U)) & 1U) expected[j] ^= lanes[i];
    }
    expanded = siqs_lp_combiner_expand(s, permuted, columns, lanes);
    for (i = 0; i < s->accepted; ++i) assert(expanded[i] == expected[i]);
    free(expanded); free(expected); free(lanes); free(permuted);
  }
  for (i = 0; i < ncols; ++i) free(matrix[i].data);
  free(matrix); free(nodes);
}

static void lp_combiner_kernels(void) {
  static const uint32_t swaps_after_cycles[] = {0, 7, 7, 4, 3, 2, 1, 5, 6, 3, 0, 7};
  uint64_t random_state = UINT64_C(0x814d2a673c9e5b01);
  uint32_t original[148], trial, i;
  siqs_ctx_t ctx;

  /* Prefix 3 already contains a nontrivial LP cycle. Prefix 4 replaces a
   * heavier pivot; prefix 5 replaces an equal-weight, higher-cost pivot. */
  lpc_test_context(&ctx, 5, 3, 12);
  for (i = 0; i < 12; ++i) {
    uint64_t previous_swaps = ctx.lp_combiner->swaps;
    original[i] = (uint32_t)(lpc_test_random(&random_state) & 31U)
                | (swaps_after_cycles[i] << 5);
    lpc_test_append(&ctx, original[i], i);
    if (i == 3 || i == 4) assert(ctx.lp_combiner->swaps > previous_swaps);
    lpc_test_prefix(&ctx, original, 1, &random_state);
  }
  assert(ctx.lp_combiner->swaps >= 3);
  lpc_test_release(&ctx);

  /* Exhaust all subsets of every prefix for varied 0/1/2/3-LP streams. */
  for (trial = 0; trial < 64; ++trial) {
    lpc_test_context(&ctx, 5, 7, 12);
    for (i = 0; i < 12; ++i) {
      uint32_t j, labels = (uint32_t)(lpc_test_random(&random_state) % 4U);
      original[i] = (uint32_t)(lpc_test_random(&random_state) & 31U);
      for (j = 0; j < labels; ++j)
        original[i] ^= UINT32_C(1) << (5U + lpc_test_random(&random_state) % 7U);
      lpc_test_append(&ctx, original[i], i);
      lpc_test_prefix(&ctx, original, 1, &random_state);
    }
    lpc_test_release(&ctx);
  }

  /* More than 128 outputs exercise multiple 64-cycle materialization blocks,
   * the high lane bit, partial last blocks, and original indices above 127. */
  lpc_test_context(&ctx, 5, 7, 148);
  for (i = 0; i < 148; ++i) {
    uint32_t j, labels = (uint32_t)(lpc_test_random(&random_state) % 4U);
    original[i] = (uint32_t)(lpc_test_random(&random_state) & 31U);
    for (j = 0; j < labels; ++j)
      original[i] ^= UINT32_C(1) << (5U + lpc_test_random(&random_state) % 7U);
    lpc_test_append(&ctx, original[i], i);
  }
  assert(ctx.lp_combiner->cycle_count > 128);
  lpc_test_prefix(&ctx, original, 0, &random_state);
  lpc_test_release(&ctx);
}
