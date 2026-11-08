/* Shared large-prime parity combination. Included by siqs.c.
 *
 * A sparse echelon basis represents the LP map on the accepted relations.
 * Reducing a new vector to zero emits an independent kernel vector. Only
 * LP parity is used here; original roots and exponents stay immutable.
 * This implementation is derived from the GF(2) formulation and our owned
 * relation/provenance code, without using another implementation.
 * Copyright (c) 2026 Dana Jacobsen. */

typedef struct {
  uint32_t left, right, cost;
} siqs_lp_node_t;

typedef struct {
  uint32_t *data, weight, node;
} siqs_lp_basis_t;

typedef struct siqs_lp_combiner {
  uint64_t *labels;
  uint32_t *table, label_count, label_alloc, table_alloc, table_grow_at;
  siqs_lp_basis_t *basis;
  siqs_lp_node_t *nodes;
  siqs_raw_arena_t originals; /* Collector-owned immutable relation storage. */
  uint32_t *cycles;
  uint32_t accepted, rank, node_count, node_alloc, cycle_count, cycle_alloc;
  uint32_t max_weight;
  uint64_t basis_entries, xor_entries, swaps;
} siqs_lp_combiner_t;

/* Packed originals use 27 row bits and five exponent bits. An entire
 * record stays wide if either field would overflow. Only general-combiner
 * originals use this layout; the ordinary 1LP path is unchanged. */
#define SIQS_LP_ARCHIVE_ROW_BITS 27U
#define SIQS_LP_ARCHIVE_ROW_MASK ((UINT32_C(1) << SIQS_LP_ARCHIVE_ROW_BITS) - 1U)
#define SIQS_LP_ARCHIVE_EXP_MAX (UINT32_MAX >> SIQS_LP_ARCHIVE_ROW_BITS)

static INLINE int siqs_lp_archive_factor_fits(uint32_t row, uint32_t exponent) {
  return row <= SIQS_LP_ARCHIVE_ROW_MASK && exponent <= SIQS_LP_ARCHIVE_EXP_MAX;
}

static INLINE uint32_t siqs_lp_archive_row(const siqs_full_relation_t *r, uint32_t i) {
  if (r->lp_packed) return ((const uint32_t *)r->factors)[i] & SIQS_LP_ARCHIVE_ROW_MASK;
  return r->factors[i].row;
}

static INLINE uint32_t siqs_lp_archive_exponent(const siqs_full_relation_t *r, uint32_t i) {
  if (r->lp_packed) return ((const uint32_t *)r->factors)[i] >> SIQS_LP_ARCHIVE_ROW_BITS;
  return r->factors[i].exponent;
}

static void siqs_lp_combiner_init(siqs_ctx_t *ctx) {
  if (ctx->lp_combiner != NULL || ctx->buffer_relations)
    croak("SIQS LP combination: invalid collector ownership");
  ctx->lp_combiner = siqs_calloc(1, sizeof(*ctx->lp_combiner));
}

static void siqs_lp_combiner_clear(siqs_ctx_t *ctx) {
  siqs_lp_combiner_t *s = ctx->lp_combiner;
  uint32_t i;
  if (s == NULL) return;
  for (i = 0; i < s->label_count; ++i) free(s->basis[i].data);
  free(s->labels); free(s->table); free(s->basis);
  siqs_raw_arena_clear(&s->originals);
  free(s->nodes); free(s->cycles); free(s);
  ctx->lp_combiner = NULL;
}

static void siqs_lp_combiner_rehash(siqs_lp_combiner_t *s, uint32_t capacity) {
  uint32_t i, *table = siqs_calloc(capacity, sizeof(*table));
  for (i = 0; i < s->label_count; ++i) {
    uint32_t pos = siqs_graph_hash(s->labels[i], capacity - 1U);
    while (table[pos]) pos = (pos + 1U) & (capacity - 1U);
    table[pos] = i + 1U;
  }
  free(s->table); s->table = table; s->table_alloc = capacity;
  s->table_grow_at = capacity - capacity / 4U;
}

/* Large primes are normally coprime to n. Preserve the graph's early
 * factor discovery even though deferred combinations need no inversions. */
static void siqs_lp_check_divisor(siqs_ctx_t *ctx, uint64_t label) {
  mpz_t divisor;
  if (label <= ULONG_MAX) {
    if (!mpz_divisible_ui_p(ctx->n, (unsigned long)label)) return;
    mpz_init(divisor);
    siqs_mpz_set_u64(divisor, label);
  } else {
    mpz_init(divisor);
    siqs_mpz_set_u64(divisor, label);
    if (!mpz_divisible_p(ctx->n, divisor)) { mpz_clear(divisor); return; }
  }
  if (siqs_insert_divisor(ctx->result, divisor)) ctx->factor_found = 1;
  mpz_clear(divisor);
}

static uint32_t siqs_lp_combiner_label(siqs_ctx_t *ctx, uint64_t label) {
  siqs_lp_combiner_t *s = ctx->lp_combiner;
  uint32_t pos;
  if (label <= 1 || label > ctx->params.large_prime_bound)
    croak("SIQS LP combination: invalid large-prime label");
  if (s->table_alloc == 0) siqs_lp_combiner_rehash(s, 1024);
  if (s->label_count >= s->table_grow_at)
    siqs_lp_combiner_rehash(s, siqs_capacity_double(s->table_alloc));
  pos = siqs_graph_hash(label, s->table_alloc - 1U);
  while (s->table[pos]) {
    uint32_t id = s->table[pos] - 1U;
    if (s->labels[id] == label) return ctx->params.fb_size + 1U + id;
    pos = (pos + 1U) & (s->table_alloc - 1U);
  }
  if (s->label_count >= UINT32_MAX - 1U - ctx->params.fb_size)
    croak("SIQS LP combination: large-prime row index exhausted");
  if (s->label_count == s->label_alloc) {
    uint32_t old = s->label_alloc;
    s->label_alloc = old ? siqs_capacity_double(old) : 1024;
    s->labels = siqs_realloc(s->labels,
        siqs_array_size(s->label_alloc, sizeof(*s->labels)));
    s->basis = siqs_realloc(s->basis,
        siqs_array_size(s->label_alloc, sizeof(*s->basis)));
    memset(s->basis + old, 0,
        (size_t)(s->label_alloc - old) * sizeof(*s->basis));
  }
  siqs_lp_check_divisor(ctx, label);
  s->labels[s->label_count] = label;
  s->table[pos] = ++s->label_count;
  return ctx->params.fb_size + s->label_count;
}

/* Leaf nodes have right == UINT32_MAX and left == original relation index.
 * XOR nodes refer only to older nodes, so expansion is iterative and exact.
 * cost bounds leaf occurrences for tie-breaking and size diagnostics. */
static uint32_t siqs_lp_combiner_node(siqs_lp_combiner_t *s,
                                     uint32_t left, uint32_t right) {
  uint32_t id = s->node_count;
  uint64_t cost = 1;
  if (id == UINT32_MAX) croak("SIQS LP combination: provenance index exhausted");
  if (right != UINT32_MAX) {
    if (left >= id || right >= id || left == right)
      croak("SIQS LP combination: invalid XOR history");
    cost = (uint64_t)s->nodes[left].cost + s->nodes[right].cost;
  }
  if (id == s->node_alloc) {
    s->node_alloc = s->node_alloc ? siqs_capacity_double(s->node_alloc) : 1024;
    s->nodes = siqs_realloc(s->nodes,
        siqs_array_size(s->node_alloc, sizeof(*s->nodes)));
  }
  s->nodes[id].left = left; s->nodes[id].right = right;
  s->nodes[id].cost = cost > UINT32_MAX ? UINT32_MAX : (uint32_t)cost;
  ++s->node_count;
  return id;
}

static void siqs_lp_combiner_cycle(siqs_lp_combiner_t *s, uint32_t node) {
  if (s->cycle_count == s->cycle_alloc) {
    s->cycle_alloc = s->cycle_alloc ? siqs_capacity_double(s->cycle_alloc) : 1024;
    s->cycles = siqs_realloc(s->cycles,
        siqs_array_size(s->cycle_alloc, sizeof(*s->cycles)));
  }
  s->cycles[s->cycle_count++] = node;
}

/* Take an already sorted LP parity vector. Keeping the largest LP ID as
 * pivot makes each XOR strictly decrease the pivot. Prefer a lighter basis
 * representative when one arrives; swapping basis and work is invertible.
 * Basis vectors plus emitted cycles stay an independent basis of all inputs.
 * In particular, rank + cycle_count == accepted at every insertion. */
static void siqs_lp_combiner_insert(siqs_lp_combiner_t *s, uint32_t first_lp,
                                    uint32_t original,
                                    const uint32_t *rows, uint32_t weight) {
  uint32_t node, i, *data = siqs_malloc(siqs_array_size(weight, sizeof(*data)));
  if (original != s->accepted)
    croak("SIQS LP combination: accepted relation order changed");
  for (i = 0; i < weight; ++i) {
    if (rows[i] < first_lp || rows[i] - first_lp >= s->label_count ||
        (i != 0 && rows[i-1U] >= rows[i]))
      croak("SIQS LP combination: invalid LP parity vector");
    data[i] = rows[i];
  }
  node = siqs_lp_combiner_node(s, original, UINT32_MAX);
  ++s->accepted;
  while (weight != 0) {
    uint32_t used = 0, a = 0, b = 0, *merged;
    siqs_lp_basis_t *pivot = s->basis + data[weight - 1U] - first_lp;
    if (weight > s->max_weight) s->max_weight = weight;
    if (pivot->weight == 0) {
      pivot->data = data; pivot->weight = weight; pivot->node = node;
      s->basis_entries += weight; ++s->rank;
      return;
    }
    if (weight < pivot->weight ||
        (weight == pivot->weight && s->nodes[node].cost < s->nodes[pivot->node].cost)) {
      siqs_lp_basis_t old = *pivot;
      pivot->data = data; pivot->weight = weight; pivot->node = node;
      s->basis_entries -= old.weight; s->basis_entries += weight;
      data = old.data; weight = old.weight; node = old.node;
      ++s->swaps;
    }
    s->xor_entries += (uint64_t)weight + pivot->weight;
    merged = siqs_malloc(siqs_array_size(siqs_size_add(weight, pivot->weight), sizeof(*merged)));
    while (a < weight && b < pivot->weight) {
      if (data[a] < pivot->data[b]) merged[used++] = data[a++];
      else if (pivot->data[b] < data[a]) merged[used++] = pivot->data[b++];
      else { ++a; ++b; }
    }
    while (a < weight) merged[used++] = data[a++];
    while (b < pivot->weight) merged[used++] = pivot->data[b++];
    node = siqs_lp_combiner_node(s, node, pivot->node);
    free(data); data = merged; weight = used;
  }
  free(data);
  siqs_lp_combiner_cycle(s, node);
}

static void siqs_lp_combiner_add_index(siqs_ctx_t *ctx, uint32_t original) {
  siqs_lp_combiner_t *s = ctx->lp_combiner;
  const siqs_full_relation_t *r = ctx->full[original];
  uint32_t rows[3], i, used = 0, first_lp = ctx->params.fb_size + 1U;
  for (i = 0; i < r->nfactors; ++i)
    if (siqs_lp_archive_row(r, i) >= first_lp && (siqs_lp_archive_exponent(r, i) & 1U)) {
      if (used == 3) croak("SIQS LP combination: input has too many odd LPs");
      rows[used++] = siqs_lp_archive_row(r, i);
    }
  siqs_lp_combiner_insert(s, first_lp, original, rows, used);
}

static void siqs_lp_combiner_add(siqs_ctx_t *ctx) {
  siqs_lp_combiner_add_index(ctx, ctx->full_count - 1U);
}

static uint32_t siqs_relation_count(const siqs_ctx_t *ctx) {
  return ctx->lp_combiner != NULL ? ctx->lp_combiner->cycle_count : ctx->full_count;
}

/* Expand 64 kernel/dependency vectors together. Cycle IDs remain meaningful
 * after later insertions and basis replacements: history is append-only. */
static uint64_t *siqs_lp_combiner_expand(const siqs_lp_combiner_t *s,
                    const la_col_t *columns, unsigned long ncols,
                    const uint64_t *lanes) {
  uint64_t *nodes = siqs_calloc(s->node_count, sizeof(*nodes));
  uint64_t *originals = siqs_calloc(s->accepted, sizeof(*originals));
  unsigned long col;
  uint32_t i;
  for (col = 0; col < ncols; ++col) {
    if (columns[col].orig >= s->node_count)
      croak("SIQS LP combination: invalid cycle provenance");
    nodes[columns[col].orig] ^= lanes[col];
  }
  for (i = s->node_count; i != 0; --i) {
    const siqs_lp_node_t *n = s->nodes + i - 1U;
    uint64_t bits = nodes[i-1U];
    if (bits == 0) continue;
    if (n->right == UINT32_MAX) originals[n->left] ^= bits;
    else { nodes[n->left] ^= bits; nodes[n->right] ^= bits; }
  }
  free(nodes);
  return originals;
}

/* Evaluate FZ in blocks of 64 cycle vectors, with bounded workspace and no
 * GMP products or permanently expanded relation sets. LP exponents are
 * checked again when the final dependencies reach square-root extraction. */

/* Parent IDs are greater than child IDs. A descending bitmap cursor is a
 * sparse reverse-topological queue: scan 64 nodes per empty bitmap word,
 * process only set bits, and leave all masks zero for the following panel. */
typedef struct {
  uint64_t *bits, *pending;
  uint32_t highest;
} siqs_lp_matrix_work_t;

static void siqs_lp_matrix_work_init(siqs_lp_matrix_work_t *w, uint32_t count) {
  w->bits = siqs_calloc(count, sizeof(*w->bits));
  w->pending = siqs_calloc(count/64U + (count%64U != 0), sizeof(*w->pending));
  w->highest = UINT32_MAX;
}
static INLINE void siqs_lp_matrix_push(siqs_lp_matrix_work_t *w,
                                        uint32_t id, uint64_t bits) {
  uint32_t word = id/64U;
  w->bits[id] ^= bits;
  w->pending[word] |= UINT64_C(1)<<(id%64U);
  if (w->highest == UINT32_MAX || word > w->highest) w->highest = word;
}
static uint32_t siqs_lp_matrix_pop(siqs_lp_matrix_work_t *w) {
  uint32_t bit;
  /* Unsigned decrement from zero intentionally reaches the empty sentinel. */
  while (w->highest != UINT32_MAX && w->pending[w->highest] == 0) --w->highest;
  if (w->highest == UINT32_MAX) return UINT32_MAX;
  bit = 63U - (uint32_t)siqs_clz64(w->pending[w->highest]);
  w->pending[w->highest] &= ~(UINT64_C(1)<<bit);
  return w->highest*64U + bit;
}
static void siqs_lp_matrix_project(siqs_ctx_t *ctx, siqs_lp_matrix_work_t *w,
    uint32_t start, uint32_t count, uint64_t *fb, uint64_t *touched) {
  siqs_lp_combiner_t *s = ctx->lp_combiner;
  uint32_t i, id;
  for (i = 0; i < count; ++i)
    siqs_lp_matrix_push(w, s->cycles[start+i], UINT64_C(1)<<i);
  while ((id = siqs_lp_matrix_pop(w)) != UINT32_MAX) {
    const siqs_lp_node_t *n = s->nodes + id;
    uint64_t bits = w->bits[id];
    w->bits[id] = 0;
    if (bits == 0) continue;
    if (n->right != UINT32_MAX) {
      siqs_lp_matrix_push(w, n->left, bits);
      siqs_lp_matrix_push(w, n->right, bits);
    } else {
      const siqs_full_relation_t *r = ctx->full[n->left];
      for (i = 0; i < r->nfactors; ++i) {
        uint32_t row = siqs_lp_archive_row(r, i);
        if (row > ctx->params.fb_size) break;
        if (siqs_lp_archive_exponent(r, i) & 1U) {
          fb[row] ^= bits;
          if (touched != NULL) touched[row/64U] |= UINT64_C(1)<<(row%64U);
        }
      }
    }
  }
}
static la_col_t *siqs_lp_combiner_matrix(siqs_ctx_t *ctx,
                                  unsigned long *nrows, unsigned long *ncols) {
  siqs_lp_combiner_t *s = ctx->lp_combiner;
  la_col_t *columns = siqs_calloc(s->cycle_count, sizeof(*columns));
  uint64_t *fb = siqs_calloc(ctx->params.fb_size + 1U, sizeof(*fb));
  uint32_t start, c, row;
  siqs_lp_matrix_work_t work;
  uint64_t *touched = NULL;
  memset(&work, 0, sizeof(work));
  siqs_lp_matrix_work_init(&work, s->node_count);
  *nrows = ctx->params.fb_size + 1U; *ncols = s->cycle_count;
  touched = siqs_calloc((*nrows + 63U)/64U, sizeof(*touched));
  for (c = 0; c < s->cycle_count; ++c) columns[c].orig = s->cycles[c];
  for (start = 0; start < s->cycle_count; ) {
    uint32_t count = s->cycle_count - start, i;
    if (count > 64) count = 64;
    siqs_lp_matrix_project(ctx, &work, start, count, fb, touched);
    for (c = 0; c < (*nrows + 63U)/64U; ++c) {
      uint64_t rows = touched[c];
      while (rows != 0) {
        uint64_t bits;
        row = 64U*c + (uint32_t)siqs_ctz64(rows);
        rows &= rows - 1U;
        bits = fb[row];
        while (bits != 0) {
          i = (uint32_t)siqs_ctz64(bits);
          ++columns[start + i].weight; bits &= bits - 1U;
        }
      }
    }
    for (i = 0; i < count; ++i) {
      la_col_t *col = columns + start + i;
      col->data = siqs_malloc(siqs_array_size(col->weight, sizeof(*col->data)));
      col->weight = 0;
    }
    for (c = 0; c < (*nrows + 63U)/64U; ++c) {
      uint64_t rows = touched[c];
      while (rows != 0) {
        uint64_t bits;
        row = 64U*c + (uint32_t)siqs_ctz64(rows);
        rows &= rows - 1U;
        bits = fb[row];
        while (bits != 0) {
          la_col_t *col;
          i = (uint32_t)siqs_ctz64(bits);
          col = columns + start + i;
          col->data[col->weight++] = row; bits &= bits - 1U;
        }
        fb[row] = 0;
      }
      touched[c] = 0;
    }
    start += count;
  }
  free(work.bits); free(work.pending); free(touched); free(fb);
  return columns;
}

/* The collector retains originals, but scheduling and journal statistics
 * count emitted LP-free combinations and independent LP pivots. */
static uint32_t siqs_partial_count(const siqs_ctx_t *ctx) {
  return ctx->lp_combiner != NULL ? ctx->lp_combiner->rank
                                : ctx->raw_count + ctx->one_lp.count;
}

static uint32_t siqs_matrix_rows(const siqs_ctx_t *ctx) {
  uint32_t extra = ctx->lp_combiner != NULL ? ctx->lp_combiner->label_count : 0;
  if (extra > UINT32_MAX - 1U - ctx->params.fb_size)
    croak("SIQS LP combination: too many matrix rows");
  return ctx->params.fb_size + 1U + extra;
}

static uint64_t siqs_matrix_prime(const siqs_ctx_t *ctx, uint32_t row) {
  if (row == 0 || row >= siqs_matrix_rows(ctx)) croak("SIQS: invalid prime row");
  if (row <= ctx->params.fb_size) return ctx->fb[row - 1U].p;
  return ctx->lp_combiner->labels[row - 1U - ctx->params.fb_size];
}

static void siqs_lp_prepare_dependency(siqs_ctx_t *ctx) {
  uint32_t rows = siqs_matrix_rows(ctx);
  if (rows <= ctx->factor_row_alloc) return;
  if (ctx->factor_touched_count != 0) croak("SIQS LP combination: growing live dependency scratch");
  ctx->factor_counts = (uint32_t *)siqs_realloc(ctx->factor_counts,
      siqs_array_size(rows, sizeof(*ctx->factor_counts)));
  memset(ctx->factor_counts + ctx->factor_row_alloc, 0,
      (size_t)(rows - ctx->factor_row_alloc) * sizeof(*ctx->factor_counts));
  ctx->factor_touched = (uint32_t *)siqs_realloc(ctx->factor_touched,
      siqs_array_size(rows, sizeof(*ctx->factor_touched)));
  ctx->factor_row_alloc = rows;
}

/* Archive original exponents, including repeated LPs, independently of the
 * LP combination history for square-root reconstruction. */
static void siqs_archive_lp_relation(siqs_ctx_t *ctx, siqs_raw_relation_t *raw) {
  siqs_full_relation_t *column;
  uint64_t labels[3] = {raw->lp1, raw->lp2, 1};
  siqs_factor_t tail[3];
  uint32_t i, ntail = 0, count, packed = 1;
  size_t bytes;
  for (i = 0; i < 3; ++i) if (labels[i] > 1) {
    uint32_t row = siqs_lp_combiner_label(ctx, labels[i]), j = ntail;
    while (j != 0 && tail[j - 1U].row > row) { tail[j] = tail[j - 1U]; --j; }
    tail[j].row = row; tail[j].exponent = 1; ++ntail;
  }
  count = 0;
  for (i = 0; i < ntail; ++i) {
    if (count != 0 && tail[count - 1U].row == tail[i].row)
      tail[count - 1U].exponent += tail[i].exponent;
    else tail[count++] = tail[i];
  }
  ntail = count;
  for (i = 0; i < raw->nfactors; ++i)
    if (!siqs_lp_archive_factor_fits(siqs_raw_factor_row(raw, i), siqs_raw_factor_exponent(raw, i))) packed = 0;
  for (i = 0; i < ntail; ++i)
    if (!siqs_lp_archive_factor_fits(tail[i].row, tail[i].exponent)) packed = 0;
  if (raw->nfactors > UINT32_MAX - ntail) croak("SIQS LP archive: too many factors");
  count = raw->nfactors + ntail;
  bytes = siqs_array_size(count, packed ? sizeof(uint32_t) : sizeof(siqs_factor_t));
  column = siqs_raw_arena_alloc(&ctx->lp_combiner->originals, siqs_size_add(sizeof(*column), bytes));
  mpz_init(column->y);
  mpz_swap(column->y, raw->y);
  column->factors = (siqs_factor_t *)(column + 1);
  column->nfactors = count; column->lp_packed = packed;
  for (i = 0; i < count; ++i) {
    uint32_t row = i < raw->nfactors ? siqs_raw_factor_row(raw, i) : tail[i - raw->nfactors].row;
    uint32_t exponent = i < raw->nfactors ? siqs_raw_factor_exponent(raw, i) : tail[i - raw->nfactors].exponent;
    if (packed) ((uint32_t *)column->factors)[i] = row | (exponent << SIQS_LP_ARCHIVE_ROW_BITS);
    else { column->factors[i].row = row; column->factors[i].exponent = exponent; }
  }
  siqs_raw_relation_free(ctx, raw);
  siqs_store_full(ctx, column);
  {
    uint32_t before = ctx->lp_combiner->cycle_count;
    siqs_lp_combiner_add(ctx);
    if (ctx->lp_combiner->cycle_count != before) {
      uint32_t node = ctx->lp_combiner->cycles[before];
      siqs_journal_full(ctx, ctx->lp_combiner->nodes[node].cost);
    }
  }
}
