# SIQS relation journal, version 2

The msiqs front end can save and resume its relation collection.
The format is implemented in `siqs_journal_inc.c`. Journal support is always compiled; creating a journal is optional. This format currently
supports serial and pthreads restart, with one coordinator owning the file.

Journals are specific to the release that writes them. This release reads
and writes version 2 only, for both ordinary and TLP collection. Earlier
versions are rejected. Use the matching older checkout to read an older
journal; no compatibility reader or converter is included. The format and
resolved policies may change again during this period of development.


## Use and inspect

From the repository root, create a journal and resume it with:

```sh
./msiqs -t 8 --journal job.journal 22095311209999409685885162322219
./msiqs -t 8 --journal job.journal
python3 tools/journal-info.py --verify-relations job.journal
python3 tools/journal-info.py --follow --eta job.journal
```

Use the same policy selection on the initial command and every restart.
The default selects 3LP for post-trial cofactors from 361 bits; if the
initial command used `--2lp` or `--3lp`, repeat that option on restart.
A saved context must match the selected policy exactly.

The Python 3.8+ inspector uses only the standard library. `--eta` prints one
initial header followed by compact progress blocks. `--follow` exits after
the final job record; an interrupted writer remains available for restart.
`--json` provides structured information, including RNG checkpoints and byte
accounting. `--strict` rejects an incomplete final frame.

For the private POSIX development checks, run `make check-journal`.
Their binaries and logs go to a reported temporary directory. Set
`SIQS_JOURNAL_TEST_DIR` to choose a different artifact directory.


## Operation

`--journal FILE N` binds one journal to one positive original input: it creates
a missing file or validates and replays an existing one. `--journal FILE`
without N requires an existing journal and obtains N from its checked header;
it never reads stdin. `--journal FILE -` reads exactly one N from stdin.
Multiple inputs are rejected in journal mode. Without the journal option,
the ordinary command-line/multiple-input/stdin behavior is unchanged.
An input mismatch is a fatal error, including when the new input would be
handled entirely by trial division, primality testing, or perfect-power handling.

Existing journals fall back to read-only access when write access is denied
or the filesystem is read-only. Completed journals can then be replayed.
Any required append, including a missing final JOB_END record, fails with a
read-only error before modifying the file or truncating an incomplete tail.

The coordinator owns the file and its buffer. Workers neither read nor write
it.  Serial and parallel collection use the same format, and the requested
worker count may change on restart. On POSIX, writable sessions take an
exclusive, nonblocking process lock; read-only sessions take a shared lock.
Both exclude concurrent writers. The caller must also keep one
coordinator per session and one session per file within a process; process
locks do not serialize callers within a single process.

Reading proceeds sequentially once.  A one-frame lookahead is retained where
necessary, not the complete journal.  No new frame is appended until that
read reaches EOF.  Completed recovery attempts and smaller-cofactor calls are
replayed in their recorded order.  This permits a journal to cover the front
end's entire pending-cofactor stack, including repeated cofactors.

Raw relations are recorded **after fingerprint duplicate rejection and before
relation-graph processing**.  Smooth relations, 1LP, 2LP, and 3LP partials, and
the partial that closes a cycle all follow the same path.  Replaying these records
through the ordinary acceptance function rebuilds fingerprints, the shared
2LP/3LP combination stage or dedicated 1LP collector, and completed relations.
Graph links and union-find state are
not serialized.  No second collection is needed when the saved prefix already
contains enough relations to finish.


## Thoughts on journal formats

I debated trying to use an existing format, but ended up deciding to use
our own.  Using msieve or yafu formats would require more digging into
their code than I wanted, and some choices made to save space in the
journal only work with that software.  I decided to look into it myself.
Unlike YAFU and msieve, we don't use a journal by default -- it is a
command line option and the filename is given.

We include a small text header at the start to make it easy to identify
the input.  A journal contains only one input, and the program refuses
to overwrite the journal.  We made a conscious choice to store `y` explicitly
rather than reconstruct it from `(family, poly number, sieve offset)`.  That
increases journal size by roughly 20–40 bytes per relation for larger inputs.
At this early stage of development, we thought this helped make the stored
relations less dependent on the polynomial generation code.


## Bytes and framing

All fixed-width unsigned integers are **little endian**.  `u32` means four
bytes and `u64` means eight bytes.  No native C structs or GMP limb storage
are written.  The writer starts the file with a five-line ASCII header,
using LF line endings on all platforms:

```text
MPUSIQJ
Version: 2
Byte-order: little-endian
N: 1000000016000000063
--
```

`N` is the original positive input in decimal. `head -n 5 FILE` displays the
complete readable header without a parser.  FB, q, M, LP and R remain in the
binary context headers, where their values can change between attempts.

The first four lines are required in this order.  After `N`, the reader ignores
additional lines until a line containing exactly `--` followed by LF.  These
may be `#` comments, blank lines or free-form metadata; they do not override
the required fields.  The optional region is scanned without buffering and
is limited to 64 MiB.  A missing or unterminated delimiter is an error.

Immediately after `--\n` comes the self-contained binary stream.  Its 16-byte
preamble also declares the version and integer byte order:

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 8 | ASCII `MPUSIQJ` followed by a zero byte |
| 8 | 4 | format version, `u32(2)` |
| 12 | 4 | byte-order marker, `u32(0x01020304)` |

These offsets are relative to the start of the binary stream, not the file.
The reader verifies both version/byte-order declarations against the
supported format.  It requires the ASCII input, the binary INPUT magnitude,
and the requested input to agree.  A malformed, incomplete, or inconsistent
ASCII header is fatal and is never treated as a disposable final block.

The binary preamble is followed by frames.  Each frame has this 56-byte
header and then its payload:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | u32 | magic `0x4b4c424a` (bytes `JBLK`) |
| 4 | u32 | frame type, 1 through 6 |
| 8 | u64 | sequence number, starting at zero |
| 16 | u32 | payload byte length, at most 64 MiB |
| 20 | u32 | item/event count |
| 24 | u64 | writing-run counter, starting at zero |
| 32 | u64 | UTC microseconds since the Unix epoch |
| 40 | u64 | monotonic microseconds since this run's clock started |
| 48 | u32 | CRC-32 of the first 48 header bytes |
| 52 | u32 | CRC-32 of the first 52 header bytes followed by the payload |

Both checksums use the standard reflected IEEE CRC-32, polynomial `0xedb88320`,
initial value and final XOR both `0xffffffff`.  The frame checksum covers the
first 52 header bytes followed by the payload; it excludes its own field.
This is the same value as Python's `zlib.crc32(header[:52] + payload)`.
The independent header checksum is checked before trusting the payload
length, so a damaged length cannot cause a valid suffix to be discarded as
an apparently truncated final payload.

All frames written through one open writer session share its run counter.
Resumed writing uses the last complete frame's counter plus one; inspection
or completed replay does not increment it or append a marker.  Counters must
stay the same or increase by exactly one.  An incomplete final frame does not
consume a run number.  UTC and elapsed timestamps are informational: anomalies
are warnings in the inspector, not invalidations.

For a new journal the run clock starts with the initial metadata frame.
On resume it starts after replay, before new collection or solving, so the
first appended frame includes the time spent producing that frame.  Later
attempts and cofactors in the same writer session retain that time origin.
Starting the clock does not append anything or consume a run number.  Each
frame's timestamp is sampled when sealing it, before checksums and file I/O.

Rates use elapsed-time differences within one run and one context.  For the
first batch of a resumed run, the inspector uses time zero and the replayed
relation counts as its baseline.  A new context instead starts its baseline
at its context frame's timestamp.  A first batch timestamped zero has no
measurable first interval; later batches still provide rates.

The writer reads the clocks at the start of resumed work and when sealing
frames. In 3LP it also reads the monotonic clock to check whether a periodic
checkpoint is due and to reset the checkpoint timer after a flush. POSIX uses
`CLOCK_REALTIME` and `CLOCK_MONOTONIC`. Windows uses
`GetSystemTimeAsFileTime` and `QueryPerformanceCounter`. The separate
embedded edition contains no journal code or clock instrumentation.

An unsigned GMP magnitude is encoded as `u32(byte_length)` followed by that
many **big-endian magnitude bytes**.  Zero has length zero; nonzero values have
no leading zero byte.  This byte order is independent of the integer-field
byte order.  These magnitudes are unsigned and never contain a sign byte.


## Frame types

### 1: INPUT

One magnitude containing the original command-line/stdin input.  Item count
is one.  It duplicates the readable `N` line and is checked against it.  This
is the first binary frame and is mandatory.  Both prefixes and the INPUT
frame must be complete and consistent; an incomplete one is rejected.

### 2: CONTEXT

This is the collection-attempt header.  Item count is one.  It contains:

1. Post-trial cofactor `n`, as a magnitude.
2. Multiplier `k`, as `u64`.
3. Packed-factor row width, `u32(21)`.
4. Policy-name byte length (`u32`) and those bytes, without a terminator.
   For 3LP, the name ends with the ASCII compatibility tag
   `;tlp-policy-v1:r1:g1:e0:b0000000000000000`. The length includes the tag,
   and replay compares the entire field byte for byte.
5. The resolved unsigned parameter fields below, each as `u64`.
6. The resolved double parameter fields below, each encoded as its IEEE 754
   binary64 bit pattern in a little-endian `u64`.
7. Factor-base entry count (`u32`), then each entry in its exact row order as
   `u32(p)`, `u32(sqrt_kn)`, `u32(logp)`.
8. Three `u64` RNG states at the start of collection, in Lanczos, cofactor,
   polynomial-chooser order.  These are internal generator states, not an
   assertion about how the caller obtained its seed.  The current xorshift64*
   streams require nonzero states, checked in both context and batch records.

Unsigned parameter order:

```
bits, policy_first_bits, policy_last_bits, max_large_primes,
fb_size, half_interval, q_count, poly_d, target_relations,
sieve_start, sieve_free_units, sieve_start_prime_floor, fb_floor,
relation_extra, multiplier_refine_divisor, fixed_half_interval,
sieve_initial, stage1_bias, sieve_raw_scores, sieve_saturating,
sieve_score_bound, large_prime_bound, smooth_bound, tlp_cutoff
```

Binary64 parameter order:

```
lp_multiplier, residual_multiplier, sieve_start_index_exponent,
fb_coefficient, interval_scale, sieve_hit_exponent,
sieve_hit_bound_nominal, sieve_hit_residual_multiplier, sieve_hit_bound, log_scale,
tlp_smooth_bound
```

Row zero represents the sign. Factor-base entry zero supplies row one, and
so on.  The header therefore preserves the meaning of every packed row.
`k*n` is derived from the stored cofactor and multiplier.

Replay independently resolves the attempt's parameters and factor base,
then requires an exact match with those fields.  It restores the three saved
starting RNG states before regenerating A selections; caller-provided states
do not override a recorded context.  An incompatible policy or factor base
fails before new work is appended.  The format does not promise compatibility
across arbitrary future code/tuning changes.  Binary64 hosts are required
by this version.

### 3: BATCH

The payload begins with thirty-two `u64` progress values (256 bytes), in
this order:

```
selected_family_count, completed_polynomial_count,
total_candidates, split_attempts, split_square, split_siqs,
split_squfof, split_ecm, split_fail, split_rejected,
tlp_candidates, tlp_batches, tlp_gcd_one, tlp_gcd_proper, tlp_gcd_whole,
tlp_completed, tlp_rejected,
la_rng_state, cofactor_rng_state, polynomial_rng_state,
stored_relations, retained_partials, current_collection_target, full_relations,
length_1, length_2, length_3, length_4, length_5, length_6, length_7, length_8plus
```

`split_squfof` is a legacy counter. ECM uses the former `split_rho` slot,
which was always zero before ECM was introduced. Version 2 retains those
counter positions within its expanded snapshot.

The snapshot describes collection state after the batch's events have been
processed. `stored_relations` counts retained relation records.
`full_relations` counts completed relations and drives collection progress.
With the shared LP combiner, these are independent LP-free combinations,
and the two counts can differ. For ordinary collection, replay also accepts
the equal counts used by older development graph writers with the v2 layout.

For ordinary collection with the shared combiner, `retained_partials` is
the LP parity rank. For other ordinary collectors, it is the number of
retained raw partials plus dedicated 1LP anchors. The 3LP writer currently
sets this field to zero and counts its originals in `stored_relations`.

The eight length bins are diagnostic and sum to `full_relations`. Graph bins
count raw relations per completed relation; the shared LP collector uses a
conservative provenance cost that may count reused contributions more than
once. Different valid kernel bases can produce different bins. Replay checks
logical counts and the bin total and preserves the producer's histogram.
An older development graph writer can discard a cycle when a failed LP
inversion finds a factor. For a compatible v2 journal, combiner replay
allows this difference in `full_relations` only after verifying a proper
split. Replay also restores the polynomial RNG state.

The rest is an ordered stream of events.  The frame's item count is the
number of events, not just the number of relations.  Each event starts with
two bytes, in this order:

| Field | Type | Meaning |
| --- | --- | --- |
| kind | u8 | 1 = relation; 2 = A-family selection |
| info | u8 | LP count for a relation; q for an A-family |

The outer frame type remains `u32`; these are the inner BATCH event headers.

**Kind 1: relation**

Info gives the number of actual large-prime labels. Zero denotes a smooth
relation. The count must fit the context's LP mode. Exactly that many
labels follow in nondecreasing order, each greater than one and at most
the context's `large_prime_bound`. The record has no unused label slots.
Each actual label uses exactly
`max(1, ceil(bit_length(large_prime_bound)/8))` bytes, little endian.
This width is resolved once per context, not per relation.

Current SIQS collection and replay support zero through three labels. The
inspector also supports four labels in a context that allows them, including
congruence checks with `--verify-relations`. The default selects 3LP from
361 bits, with `--2lp` and `--3lp` overriding policy selection.

After the large prime labels:

- `y`, as an unsigned magnitude reduced modulo the current cofactor.
- Factor count, `u32`.
- That many delta/exponent entries as described below.

Each factor entry starts with the canonical unsigned LEB128 encoding of
`(row_delta << 1) | (exponent != 1)`.  If the low bit is
set, the exponent follows as another canonical ULEB128 value; otherwise it
is implicitly 1.  An explicitly encoded exponent must be at least 2.

The previous row starts at zero for each relation.  The first delta is the
first row itself (which may be sign row zero); subsequent deltas must be
positive.  Rows remain limited to 21 bits and exponents to 11 bits, preserving
the existing factor-vector bounds.  ULEB128 has seven payload bits per byte,
least significant group first, with bit 7 marking continuation.  Overlong,
truncated and overflowing encodings are rejected.  Factor counts are checked
before allocating vectors; decoded rows are checked against the context's
factor base before use.

The writer reserves capacity once per relation and packs directly into the
batch buffer.  A delta/flag value uses at most four bytes and an exceptional
exponent at most two; no per-factor buffer growth checks are needed.

**Kind 2: A-family selection**

- Factor count `q` is the info byte; it is not repeated in the body.
- The `q` zero-based factor-base entry indices, each `u32`, in chooser order.

The info value must match the context's q, currently between 1 and 16.

These indices determine A exactly.  Replay regenerates each chooser selection
and checks the indices, without computing B/roots or sieving.  This restores
the deterministic search cursor, its RNG where used, and the A history,
including the small q=1/2 choosers.  A changed chooser that cannot reproduce
the saved sequence is rejected rather than silently repeating old families.

Journalled collection disables Cubic where it would otherwise be used;
SAR and the existing small deterministic choosers remain.  Nonjournalled
chooser policy is unchanged.

A restart starts fresh families after the recorded frontier.  It does not
resume midway through a B-polynomial family.  A family selected before the
last committed frame can have an unsaved suffix; that suffix is abandoned.
Uncommitted events in the final in-memory batch are also lost.  Previously
committed accepted relations remain usable.

Ordinary collection flushes at 256 events or 256 KiB, whichever is reached
first. Serial collection also flushes before selecting the next family,
and the ordinary parallel coordinator flushes after merging each worker
family.

A periodic TLP checkpoint saves the coordinator's accepted relations and
graph statistics while residual jobs keep running. Pending residual work is
not serialized. Parallel TLP uses a 4 MiB byte target with a twenty-second
monotonic time target for a nonempty buffer, checked between result
publications as the coordinator processes events. This is a checkpoint target,
not a strict time limit on a worker job.

Restart discards unfinished sieve buffers and residual queues, including
partly filled LP batches that may contain candidates older than the last
checkpoint. Previously committed accepted relations remain reusable. The
recorded A-family sequence restores the chooser beyond those unfinished
families; worker buffers are not exact continuation points.

An event is never divided between frames. Matrix solves drain pending
residual work and flush accepted events before matrix construction, so those
batch timestamps exclude the subsequent linear algebra. Attempt completion
also drains pending work and flushes any remaining events.
`SIQS_JOURNAL_BATCH_BYTES` can lower the byte thresholds for development
tests; the TLP target is sixteen times the ordinary threshold. Progress
snapshots are taken at flush time. Accepted
relation totals are rebuilt from the records, not copied from counters.

### 4: ATTEMPT\_END

Item count is one. Payload:

- Success/progress flag, `u32(0)` or `u32(1)`.
- The input to the current SIQS API call, as a magnitude.
- Partition count, `u32`.
- Each partition member, as a magnitude.

The partition must multiply to that API-call input.  Its order is retained
because the front end's pending-cofactor stack must make the same subsequent
calls.  A completed unsuccessful attempt is followed by another CONTEXT if a
recovery policy was used.  An incomplete attempt ends at EOF and is resumed.

### 5: RETURN

Item count is one.  Payload is the API-call input magnitude, partition count
(`u32`), and partition magnitudes.  This records the verified API return,
including calls completed by pretests without a collection context.  The
partition may contain composites; the front end processes those with further
journaled calls.  The final printed factorization still verifies completion.

### 6: JOB\_END

Item count is one.  Payload is `u32(complete)`, followed by the original-input
magnitude, partition count (`u32`) and the final partition magnitudes.
Complete is zero or one; the partition must multiply to the original input.
The front end writes this only after processing every pending cofactor and
sorting its final factor list. Complete is one when all final factors pass
primality testing. Zero records a normal return that left an unresolved
composite.

This is the terminal record for the whole job, not an individual recovery
attempt or cofactor call.  It must follow any ATTEMPT\_END/RETURN records.
Closing a journal does not write JOB\_END.  An interrupted writer consequently
leaves no promise that the job finished.  Completed replay verifies and
consumes this record without appending another one.  A valid interrupted prefix
without JOB\_END can gain one when the front end finishes its replay.

The inspector checks that the final partition multiplies to the input and
that the completion flag is zero or one. `--follow` exits after reading
JOB\_END, including when the status reports incomplete factoring. Without
that record it keeps waiting. Complete frames after JOB\_END are
rejected.  The record does not announce the earlier instant when sieving stops.

## Interrupted writes and durability

A short final frame header or payload is ignored.  Its valid predecessor is
the end of the replayable prefix.  Before appending, the writer truncates the
incomplete tail and continues the frame sequence there.  A complete frame
with a bad checksum, invalid sequence, malformed record, or incompatible
context is a fatal error; it is not silently discarded as a tail.

Each completed frame is written and `fflush`ed synchronously by the
coordinator.  This protects against ordinary process interruption with bounded
loss of the current batch.  There is no per-frame `fsync` or Windows
`FlushFileBuffers`: this is not a power-loss durability guarantee.  CRCs
detect accidental damage; the format is not authenticated.

Opening a completed journal and replaying it does not append duplicate
relations or completion frames. The original file remains byte-identical.
Errors are fatal standalone diagnostics with a nonzero exit status.  No
fallback silently overwrites an incompatible journal.


## Read-only inspection and live writers

`tools/journal-info.py` validates the complete prefix and summarizes contexts,
runs, progress and byte use.  Its optional modular checks use the saved y and
factor vectors directly.  Graph snapshots are checked for structural consistency
but the Python tool does not reconstruct the graph. Measured raw/full rates
use monotonic progress within one context/run; UTC anomalies only warn.

Plain `--eta`, including with `--follow`, checks frame checksums and reads
batch progress snapshots without decoding individual family and relation
records. Raw relation counts come from the batch event counts and the
change in the family count. Add `--json`, `--verify-relations` or
`--strict` to use the full decoder.

Collection ETA uses a local quadratic fit to completed full relations over
the recent window (five minutes by default), anchored to the actual current
count.  The fit uses equal-duration bins to limit the influence of bursts of
frames.  It needs at least six occupied bins spanning at least one minute
(or the selected window, if shorter); otherwise it uses the measured linear
rate.  Nonpositive velocity, negative curvature, and degenerate fits also
fall back to that rate.  Positive acceleration is projected for at most one
observed window, followed by constant-rate growth.  This changes the ETA,
not the displayed measured rates or the SIQS collection code.

`--eta-model linear` selects the previous linear estimate. `--eta-horizon`
can further shorten the future acceleration interval.  JSON output includes
the selected model, linear estimate and fit information for comparison.

Human-readable ETAs say "roughly" until there is a minute of recent history
(or the selected window, if shorter), or while fewer than half the completed
relations come from combined partials.  Smooth-only operation skips the partials
condition.  Rough estimates use whole seconds or minutes; JSON retains the
unrounded forecast.  These are display cues and do not change the ETA model.

A read-only inspector takes no writer lock. POSIX factorer sessions use an
advisory fcntl write lock for writable journals and a read lock for read-only
journals. Windows factorer streams use `_SH_DENYWR`, excluding another writer
while allowing readers; locking the journal's data bytes would also block
readers on Windows.  See Microsoft's
[\_fsopen sharing modes](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fsopen-wfsopen)
and
[byte-range locking behavior](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex).
The Windows implementation still needs native runtime validation.

Live inspection accepts a complete, checksummed prefix and waits at an
incomplete final frame. A  checksum failure is retried once in follow mode
in case it overlaps replacement of an interrupted tail; a persistent failure
is an error.  Earlier complete frames are never rewritten by the writer.
