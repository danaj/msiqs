BLOCK-LANCZOS SCALING BENCHMARK
==============================

Build and run commands below assume the repository root.

Build only (does not run a large benchmark):
  make bench-lanczos

Or compile directly with a C compiler:
  cc -O3 -march=native -falign-loops=32 -DPSIQS -pthread \
    -o bench-lanczos tools/lanczos-bench.c -lm

The benchmark and solver have no GMP or SIQS host-adapter dependency.
Diagnostic verbosity is passed explicitly. Requires POSIX pthreads,
clock_gettime(CLOCK_MONOTONIC), and getrusage.

Example full-size run:
  ./bench-lanczos --max-threads 64 --repeat 3 --output lanczos-110.tsv

Quick functional/small scaling check:
  ./bench-lanczos --rows 33000 --max-threads 8 --output lanczos-small.tsv

Sweep: 1, 2, 4, 8, ... up to the maximum, including the exact maximum if not
a power of two (e.g. --max-threads 10 includes 10). Counts include the caller,
as in the production solver. The first solve is serial. Repeated sweeps
alternate ascending/descending order; the summary gives median, min/max,
median CPU, and wall speedup against the serial median. TSV records every
solve and is flushed after each result. Existing output files are not replaced.
--verbose adds the solver's iteration diagnostics. --help lists all options.

The tool defaults to 183590 rows and 183654 columns: the reduced RSA-110
dimensions recorded in misc/siqs-timing-info.txt. Generator v2 uses a broad
frequent head and approximately inverse-row tail incidence. Its rounded head
probabilities follow the captured d70/d80/RSA100 row profiles: 50% for rows 0..3,
30% for 4..15, 15% for 16..31, 13% for 32..63, 12% for 64..111, 10% for 112..191,
and 6% thereafter. The default head is min(256, rows/4). This gives materially
occupied rows beyond the first 48 deferred rows, so the packed dense group
represents useful work. The old head64 generator left mostly sparse rows
there and understated packing's benefit.

Each column has one shared, right-skewed intensity: approximately
3/8 + (5/2)*U*V for independent uniform U,V. It scales both the tail target
and head probabilities; the first four head rows retain 50% incidence. Thus
heavy columns tend to have denser heads too. The tail mean is approximately
--weight (default 50), with possible targets from roughly 3/8 to 23/8 of it,
rounded and bounded below by two. Default columns average about 80 entries,
with a broader weight distribution than the old 50+/-16 tail. The head profile,
weight spread and correlations approximate the captured matrices; these are
generated workloads, not actual relations or exact rank-structure models.

Tail hits choose logarithmic row buckets, with one eighth uniform sampling.
Two distinct coverage hits per column ensure each tail row occurs at least
twice. Entries are distinct within each column and stored in exact-size
allocations; columns are ordered as after production's reduction. Matrix
dimensions remain rows+64 columns, and no dependencies are planted. All
sampling uses integer arithmetic and is deterministic for a given seed.
--weight changes tail density, --head overrides the frequent-head size, and
--seed generates another matrix. For example, compare --weight 35, 50, 70 to
test density sensitivity. The requested mean must fit the tail; sampled
targets are capped at the tail size for small or unusually dense matrices.
Generation is not timed.

The log identifies generator v2 and reports the column-weight range/standard
deviation and actual ranked incidence in rows 0..47 and 48..111. The latter
64-row band contributes about 7.8 entries/column by default, compared with
6.6, 13.3, 7.3 in the three captures. Synthetic results remain a useful size
sweep; --matrix supplies exact real workloads for checking conclusions.

Timed work includes the normal matrix conversion, pool creation, complete
block-Lanczos iteration, dependency extraction/verification inside the solver,
retries, and pool/storage teardown. It does not include matrix generation or
the harness's independent validation. Each solve uses identical matrix data
and the fixed SplitMix64 seed 0x83d2e5b79a4c610f. The local RNG state advances
across solver retries. Outside the timer, the harness checks A*X=0, nonempty and
independent dependency lanes, and bit-for-bit equality with the serial result.
This finds dependencies, not integer factors.

The harness includes lanczos.c to observe actual thread count and execute its
unchanged private recurrence. Its small setup/retry wrapper mirrors the public
entry point. No production sources, algorithms, or pool policies are changed.
The BENCHMARK build defaults NLA_MAX_THREADS to 256 so a 128/192-core machine
can explore higher counts; production's provisional 32-thread cap is unchanged.
Override the harness cap at compilation with -DNLA_MAX_THREADS=N if desired.
Out-of-range requests are rejected. Actual pool size is recorded; a packed
small matrix (at most 32768 columns) or failed pool creation is clearly marked
as serial fallback, not misleadingly labelled parallel. A summary actual count
of zero means pool size varied between repeats; consult the per-solve TSV.

These are solver scaling results, not predictions of overall factoring time.
Synthetic matrices cannot capture every QS incidence/rank/cache peculiarity.
For a reliable choice, use several seeds/densities on the target hardware,
with no competing work. Large thread counts allocate one private row buffer
per solver worker: at the default size, roughly 1.4 MiB per worker in addition
to shared matrix/solver storage. Oversubscribed CPUs and NUMA placement can
strongly change results. The tool does not pin threads or select a production
thread limit automatically.

Exact reduced matrices
----------------------

Use --matrix FILE to benchmark actual reduced columns without generation:
  ./bench-lanczos --matrix /private/tmp/d80-s1.matrix --max-threads 8 --repeat 3 --output real-matrix.tsv

--matrix rejects explicit --rows, --weight, --head, and --seed. Existing
--max-threads, --repeat, --output, and --verbose options remain available.
The stored row and column counts are used exactly, including empty rows and
column counts that differ from rows + 64. Columns retain their supplied order,
weights, original identifiers, and repeated indices. The solver still uses
its fixed RNG seed and independently verifies all dependency vectors.

LCMAT001 is a regular binary file: eight ASCII bytes LCMAT001, followed by
little-endian uint32 row count, column count, and dense-row count (must be zero).
Each ordered column contains uint32 weight, original column identifier, then
weight uint32 row indices. Dimensions must be nonzero; indices must be less
than the row count. Invalid magic, bitmap rows, allocation-size overflow,
truncation, and trailing bytes are rejected before solving. This is the output
format of the disposable reduced-matrix capture tools; no journal replay or
SIQS work is performed by this benchmark.

Loading and hashing happen before solver timers. The log records the path,
exact dimensions, entry count, and FNV-1a 64-bit hash of every input byte.
TSV tail_weight, head, and seed are zero for --matrix inputs because these
synthetic-generation parameters do not apply. The TSV columns are unchanged.
The default synthetic generator and its options retain their existing behavior.
