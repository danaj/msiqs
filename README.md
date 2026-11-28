# msiqs

msiqs is a portable C implementation of the Self-Initializing Quadratic
Sieve for integer factoring, using GMP for large-integer arithmetic.
It began in Math::Prime::Util::GMP and is now developed independently.
Dana Jacobsen wrote it in late 2026, using OpenAI Codex to write parts
of the code, tests, and tools.

Both SIQS and the block Lanczos solver can be embedded in another C
project in addition to their use in the included standalone program.
Optional pthreads accelerate both relation collection and block Lanczos
linear algebra.  The sieve and matrix solvers use portable C without
hand-written assembly or explicit SIMD intrinsics.

## Installation and use

Building requires a C compiler and GMP (6.2 or newer recommended).
The Makefile uses Perl.  The direct compiler command requires only
a C compiler and GMP.

### Standalone program

From the repository root:

```sh
make
./msiqs 22095311209999409685885162322219
```

With a pthread build, request eight workers and periodic progress reports:

```sh
./msiqs -t 8 -vv 22095311209999409685885162322219
```

The Makefile detects pthread support automatically.  The default is one
factoring thread, and very small inputs may stay serial even when more
are requested.  `-v` shows the policy range, sieve parameters, collection
and solve timings, and the outcome of each solve. Collection summaries give
accepted originals per second for the round.
`-vv` adds roughly minute-by-minute progress
during collection and long Lanczos runs.  Collection progress distinguishes
LP-free relations ready for the matrix from all accepted originals.
Use `-vvv` for the policy name, the work-based collection trace, and
detailed tuning, LP, and solver diagnostics. Use `-vvvv` to trace
matrix readiness checks during collection.
With no integers on the command line, `msiqs` reads whitespace or newline
separated inputs from stdin.
See [build and API notes](docs/IMPLEMENTATION.md#build-and-api-notes) for
compiler options and serial-only builds.

Three-large-prime collection is selected automatically for post-trial
cofactors of 361 bits and larger.  Smaller cofactors use the ordinary policies.

The command-line options override that selection:

```sh
./msiqs --2lp -t 8 -vv INTEGER
./msiqs --3lp -t 8 -vv INTEGER
```

`--2lp` forces the ordinary policies. `--3lp` selects the separate `3lp_...`
policy table for post-trial cofactors of 270 through 512 bits.  Uncovered
sizes use the ordinary policies.  Recovery policies and internal
cofactor-splitter calls retain their existing selection.

For large inputs, `--2lp` can reduce peak memory.  Factoring may take longer.

3LP uses batch GCDs and shared large-prime combination before constructing
the factor-base matrix.  Pthread builds enable residual-first screening by
default and recover exact factor vectors only for accepted records.
The batch cutoff trades some relation yield for memory.
Use `-vv` for collection progress and `-vvv` for diagnostic counts, including
the pending queue and batch statistics when collecting 3LP locally.
`make check-3lp` runs the component checks. `make check-3lp-integration` also
factors a 270-bit semiprime, interrupts and resumes a journal,
and verifies every saved relation and completed replay.

### Distributed collection

Build the distributed program with `make dmsiqs`. The [dmsiqs guide](DMSIQS.md)
covers workers, coordinator discovery, SSH forwarding, and the Axion tools.

### Local journals

To save relation collection and resume it later, use an optional journal:

```sh
./msiqs -t 8 --journal job.journal 22095311209999409685885162322219
./msiqs -t 8 --journal job.journal
python3 tools/journal-info.py --follow --eta job.journal
```

The [journal guide](docs/JOURNAL.md) describes restart behavior, inspection and
the file format. Journalling is off unless requested.
Use the same policy selection when restarting a journal: if the initial
command used `--2lp` or `--3lp`, repeat that option on restart. Journals are
release-specific; this release writes and reads format version 2 only.

### Embedding in C

The independent [embedded package](embedded/README.md) supplies a frozen,
serial GMP-based SIQS, a GMP-free TinySIQS128, and GF(2) matrix solvers,
with build and API instructions. Updates are manual.

The [matrix solvers](docs/LANCZOS-USE.md) can also be embedded independently,
without GMP, and are not tied to this SIQS implementation.

## SIQS Performance at a glance

All settings left at default, no tuning for machine or input.

Representative timing results on an Apple M1 Pro, using multiple
balanced semiprimes and an October 2026 SIQS build.

| Decimal digits | Median time <br> 1 thread | Median time <br> 8 threads | Peak RSS <br> 1 thread |
| ---: | ---: | ---: | ---: |
| 30 | 0.95 ms | 1.08ms | 2.8 MiB |
| 40 | 7.2 ms | 3.0ms | 3.3 MiB |
| 60 | 0.51 s | 0.089s | 11 MiB |
| 80 | 39 s | 5.6s | 66 MiB |
| 100 | 51 min | 7 min | 410 MiB |

With 8 threads on a 2021 MacBook M1 Pro, RSA-100 completed in 4m 42.8s,
RSA-110 in 50m 55.9s.

A 2026 MacBook M5 Pro with 18 threads completed RSA-100 in 2m 16.2s.

November 2026 timing results:

48-core AMD Turin (EPYC 9B45 4.1GHz, Zen5), 48 threads:

| Input    | digits | bits | Wall time | CPU time |
| :---:    | ---: | ---: | ---: | ---: |
| RSA-100  | 100  | 330 |      36.0 s | |
| RSA-110  | 110  | 364 |   5m 35s | 4.1 core hrs |
| RSA-120  | 120  | 397 |  34m 56s | 27.0 core hrs |
| RSA-130  | 130  | 430 | 5h 3m   | 241 core hrs |

With 96 threads on an Intel Xeon 8581C, RSA-100 took 31.0 s and RSA-130 took 4h 13m (379 core-hours).

See the [distributed timings](DMSIQS.md#preliminary-timings) for six Axion workers.

More comparisons can be seen in the [benchmarks document](docs/BENCHMARKS.md)

### Supported range

Parameters cover inputs through 512 bits. Complete factoring runs have
been verified through 430 bits, while the larger range has had component
testing.  The design expectation of msiqs is to factor all composite
inputs within its size range.  Composites with multiple factors are
usually fully factored, but this is not guaranteed.

msiqs implements the Self-Initializing Quadratic Sieve, using small ECM
internally to split relation cofactors.  It does not offer general-purpose
ECM or NFS factoring, which can be more practical choices for some inputs.
While it works well from 1 to at least 430 bits, typically SIQS is one part
of a generic factoring package, as in a general-purpose factoring routine, or in
[YAFU](http://sourceforge.net/projects/yafu/).

## Further reading

- [implementation details](docs/IMPLEMENTATION.md)
- [evolution and history](docs/EVOLUTION.md)
- [benchmark comparisons and methodology](docs/BENCHMARKS.md)
- [benchmark graphs](docs/graphs/).

## Checks and license

`make check` runs the SIQS checks, including the shared large-prime combiner
and the 3LP component checks. Additional targets are
`make check-embedded`, `make check-pr32`, `make check-autotune`,
`make check-journal`, and `make check-3lp-integration`. Developer checks use
core Perl and Python 3.

Current sources are [MIT licensed](LICENSE). See [source history](docs/history/README.md)
for the extraction, original commit maps, and historical notices.
