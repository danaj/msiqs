#ifndef MPU_LANCZOS_H
#define MPU_LANCZOS_H

#include "ptypes.h"

/* The panel dense solver wins through this measured reduced-column
 * crossover.  Both solvers remain valid on either side of it. */
#define LA_DENSE_CROSSOVER_COLS 1536UL

typedef struct {
  /* 32-bit indices cover the configured factor-base and matrix limits. */
  uint32_t *data;
  uint32_t weight;
  uint32_t orig;
} la_col_t;

typedef struct {
  uint64_t iterations;
  uint32_t threads, attempts;
} la_lanczos_stats_t;

/* Return the bit showing whether a column belongs to a dependency. */
static INLINE uint64_t la_get_null_entry(const uint64_t *nullrows,
                                         unsigned long column,
                                         unsigned long dependency) {
  return nullrows[column] & ((uint64_t)1 << dependency);
}

/* Peel singleton rows and trim excess columns before solving.
 * Diagnostic verbosity is caller-owned: 0 quiet, 1 summaries, 2 progress,
 * 3 detailed diagnostics. No host-global verbosity adapter is needed. */
extern void la_reduce_matrix(unsigned long *nrows,
                             unsigned long *ncols,
                             la_col_t *cols, int verbose);

/* Find up to 64 exact nullspace dependencies by dense elimination. */
extern uint64_t *la_dense_nullspace(unsigned long nrows,
                                    unsigned long ncols,
                                    const la_col_t *cols,
                                    uint64_t *mask);

/* Find nullspace dependencies with the sparse block-Lanczos solver.
 * Seed initializes a per-call SplitMix64 state carried across retries.
 * Every seed is valid, including zero; no shared RNG state is used. */
extern uint64_t *la_block_lanczos(unsigned long nrows,
                                  unsigned long dense_rows,
                                  unsigned long ncols,
                                  la_col_t *cols,
                                  uint64_t seed,
                                  uint64_t *mask, int verbose);

/* Find a wider dependency sample by retaining all rows in the iteration. */
extern uint64_t *la_block_lanczos_wide(unsigned long nrows,
                                       unsigned long dense_rows,
                                       unsigned long ncols,
                                       la_col_t *cols,
                                       uint64_t seed,
                                       uint64_t *mask, int verbose);

/* Optional diagnostics for one solve. Stats include all internal attempts
 * and the actual thread count. An empty matrix reports zero for each field.
 * Serial builds use one thread regardless of the requested count. */
extern uint64_t *la_block_lanczos_report(unsigned long nrows,
                                        unsigned long dense_rows,
                                        unsigned long ncols,
                                        const la_col_t *cols,
                                        uint64_t seed, uint64_t *mask,
                                        int retain_all_rows, int verbose,
                                        uint32_t nthreads,
                                        la_lanczos_stats_t *stats);

#ifdef PSIQS
/* Use a per-solve worker pool for large unpacked matrices; otherwise serial. */
extern uint64_t *la_block_lanczos_threaded(unsigned long nrows,
                                          unsigned long dense_rows,
                                          unsigned long ncols,
                                          la_col_t *cols,
                                          uint64_t seed,
                                          uint64_t *mask,
                                          int retain_all_rows, int verbose,
                                          uint32_t nthreads);
#endif

#endif
