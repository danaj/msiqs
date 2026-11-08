# Using the GF(2) matrix solvers

The interfaces below apply to both the frozen embedded edition and the
main project. Build examples use the embedded edition unless stated otherwise.

The solvers find subsets of matrix columns whose XOR is zero. They return
dependencies, which a factoring program can use in its square-root step.
They can also be used for binary matrices unrelated to factoring.

Compile lanczos.c with lanczos.h and ptypes.h alongside it. No GMP, libm,
128-bit type, initialization, or shutdown call is needed. For a host program
my-nullspace.c, build from the repository root with:

```sh
cc -O3 -Iembedded -o my-nullspace my-nullspace.c embedded/lanczos.c
```

## Matrix representation

Include `lanczos.h`, which defines each column as:

```c
typedef struct {
  uint32_t *data;
  uint32_t weight;
  uint32_t orig;
} la_col_t;
```

For the ordinary format, `data[0..weight-1]` contains the row indices where
that column has a one. Each index must be less than `nrows`, and occur only
once in that column. Reduce integer entries modulo two first. Sorting the
indices is useful but not required. An empty column has weight zero and may
have `data = NULL`. Row and column counts must fit in `uint32_t`.

`orig` identifies the original column; initialize it to the column number
if using the optional reducer. Solver results follow the supplied column
array's order, rather than `orig` order.

The solvers leave the columns and their data unchanged. Keep them alive
until the call returns; they may be static, stack, or heap allocations.
Separate calls can run concurrently with their own result and mask storage,
and may share an immutable input matrix.

## Calling a solver

Given `unsigned long nrows, ncols` and a `la_col_t *cols`:

```c
uint64_t mask = 0;
uint64_t seed = UINT64_C(0x83d2e5b79a4c610f);
uint64_t *deps = la_block_lanczos(nrows, 0, ncols, cols, seed, &mask, 0);
```

The second argument, `dense_rows`, is zero for ordinary row lists. The final
argument is verbosity: zero is quiet, with increasing diagnostics through
level three. Dense elimination has no verbosity argument.

For dense elimination, replace the call with:

```c
uint64_t *deps = la_dense_nullspace(nrows, ncols, cols, &mask);
```

For another Lanczos dependency sample, use `la_block_lanczos_wide` with the
same arguments as ordinary Lanczos. It retains all rows during iteration,
including frequent rows that the ordinary solver may defer until recovery.
It uses the same result format and does not guarantee more dependencies.
Free an earlier result before replacing it with another solver's output.

Choose the solver explicitly. SIQS uses dense elimination through 1536
reduced columns, then Lanczos; that measured crossover is not a correctness
limit or necessarily the best setting for another application. Dense
elimination returns up to 64 independent dependencies. Lanczos returns a
sample of up to 64, rather than the complete nullspace. Any seed is valid,
including zero; Lanczos uses a private SplitMix64 state across retries.

## Reading and releasing dependencies

A successful result contains `ncols` words. Bit `d` of `deps[c]` says whether
column `c` participates in dependency `d`. The set bits in `mask` identify
the valid dependencies:

```c
if (deps != NULL && mask != 0) {
  for (unsigned int d = 0; d < 64; d++) {
    if (!(mask & (UINT64_C(1) << d))) continue;
    printf("dependency %u:", d);
    for (unsigned long c = 0; c < ncols; c++)
      if (la_get_null_entry(deps, c, d)) printf(" %lu", c);
    putchar('\n');
  }
}
free(deps);
```

This example needs stdio.h and stdlib.h. Use ordinary `free` with a compatible
C runtime. The caller still owns the input column array and its data.
Returned dependencies are checked against the matrix; in a factoring
application, a valid dependency can still produce only trivial factors.

`NULL` with `mask = 0` means no dependencies were returned, not proof that
the nullspace is empty. Try another seed or solver, or collect more relations,
with a bound on retries. Dense elimination can also return NULL on allocation
or size failure. Other allocation/invariant failures use ptypes.h's fatal
handler; a NULL return is not a general allocation-error contract.

## Optional matrix reduction

```c
la_reduce_matrix(&nrows, &ncols, cols, 0);
```

The reducer is tailored to factoring: it peels singleton rows and trims
heavy excess columns. Trimming can discard dependencies, so skip it when
the full nullspace of a general matrix is needed.

Unlike the solvers, reduction modifies the column array and frees discarded
columns' data. Each column's data must be a separate allocation compatible
with `free`; do not pass stack arrays, static data, or slices of one shared
allocation. It compacts surviving columns into `cols[0..ncols-1]` and sets
trailing data pointers to NULL. The caller frees the surviving data and
column array when finished.

`ncols` is updated, but `nrows` remains the original row-index bound; rows
are not renumbered. Do not replace it with the printed active-row count.
Distinct `orig` values preserve the mapping: after solving, column `c` in
the result corresponds to original column `cols[c].orig`. Never reduce a
matrix while another call is using it.

## Packed dense rows

Lanczos also accepts a packed dense head when `dense_rows > 0`. After each
column's `weight` sparse row indices, append `ceil(dense_rows / 32)` uint32_t
words. Bit `r % 32` in word `r / 32` describes row `r`, for rows below
`dense_rows`. Sparse indices must then be at least `dense_rows`, and unused
high bits in the final packed word must be zero. `dense_rows` must not
exceed `nrows`.

Ordinary, wide, and threaded Lanczos accept this format. Dense elimination
and the reducer do not; expand the packed head into explicit row indices
before calling either. This is separate from automatic internal packing.

## Parallel Lanczos from the main project

Use the main project's lanczos.c, lanczos.h, ptypes.h, and planczos_inc.c,
compiling caller and solver with `-DPSIQS -pthread`. Do not compile
planczos_inc.c separately. No GMP or libm is needed. The serial entry points
remain available, and the threaded call is:

```c
uint64_t *deps = la_block_lanczos_threaded(nrows, 0, ncols, cols, seed, &mask, 0, 0, 4);
```

The last arguments are `retain_all_rows`, verbosity, and total threads.
Set `retain_all_rows = 1` for the wide behavior. The count includes the
calling thread; workers are joined before the call returns. Each call owns
its pool, so concurrent calls can oversubscribe CPU and memory.

The size/entry-count policy caps requests at 1, 4, 8, 16, or 24 workers;
the requested count remains an upper bound. For example, a request for 2
can select 2 workers. Worker selection precedes internal format selection.
Serial packing defaults to disabled on x86 and a cutoff of 18432 columns
otherwise; `-DNLA_PACK_MAX_COLS=N` overrides that cutoff. Parallel solves use
the unpacked format. Failed pool setup falls back to serial execution.

The first column counts selecting each cap are:

| Default | 4 workers | 8 workers | 16 workers | 24 workers |
| --- | ---: | ---: | ---: | ---: |
| x86 | 6144 | 12288 | 49152 | 393216 |
| Other | 6144 | 24576 | 49152 | 393216 |

Smaller matrices select one worker. Other targets also select one worker
at 450000 entries or fewer. On x86, matrices with 8192–12287 columns and
at least 900000 entries select the 8-worker cap early. Entries count original
sparse indices and set dense-head bits. These are coarse measured defaults;
the larger 16/24-worker steps come from AMD measurements, while the ARM
measurements covered requests through 8 workers.

The pool's additional hard limit is `NLA_MAX_THREADS`, normally 32; override
it when compiling the solver if needed. Each pool thread needs roughly
`8 * nrows` bytes of row scratch plus smaller buffers. For scaling
measurements, see [the Lanczos benchmark](../tools/README-lanczos-bench.txt).
