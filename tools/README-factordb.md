# FactorDB worker

`factordb.py` fetches composite inputs from FactorDB, factors them locally,
and submits verified results. It uses the Python 3 standard library and calls
the selected factoring executable.

## Configuration

From the msiqs repository root, copy the example configuration.

```sh
cp tools/factordb.example.json factordb.json
```

Set `fdb_user_token` to your 32-character FactorDB API token, or use the
`FDB_USER_TOKEN` environment variable to override that field. You can select a
different configuration file with `--config`. Executable paths and `work_dir` in
the configuration are relative to the configuration file. The example's
`_comments` object holds notes ignored by the worker.

| `method` | Executable setting | What `threads` controls |
| --- | --- | --- |
| `msiqs` | `exec_msiqs` | Threads for a single msiqs job |
| `yafu` | `exec_yafu` | Threads for a single YAFU job |
| `mpu` | `exec_mpu` | Number of persistent single-threaded workers factoring separate inputs |

`msiqs` requires a pthread build for more than one thread. YAFU jobs run in
temporary directories and use `yafu.ini` from beside the executable when present.
Relative paths to external factoring tools and NFS parameters resolve from that
file's directory.

To use `mpu`, build the executable against a Math-Prime-Util-GMP source checkout.

```sh
python3 tools/build-factordb-batch-worker.py --mpu-src ../Math-Prime-Util-GMP
```

## Running a worker

Run one batch with the configured method, or choose a method on the command line.
`--compact` groups starts by input size, such as `Factoring C71 C72 C72`.
The batch summary shows wall time per completed factorization and median job
time. Submission output includes RPC calls and HTTP requests.

```sh
python3 tools/factordb.py --once
python3 tools/factordb.py --method mpu --compact --threads 8 --min-digits 19 --max-digits 50
```

With the default `run` command, the worker resumes its saved queue. Each fetch
requests random composites one digit size at a time, starting at `min_digits`.
It stops at `max_digits` or when the queue reaches `batch_size`. Invalid items
are skipped and counted. The worker factors queued inputs, then submits the
results.

`batch_time` limits how long the worker starts jobs before submitting. Jobs
already running finish first. Zero allows every job in the batch to start.
After an underfilled fetch, `short_download_delay` sets the minimum interval
before the next fetch. Factoring and submission count toward it. The default
is 3 seconds and can be overridden with `--short-download-delay`.

You can fetch or submit saved results as separate operations.

```sh
python3 tools/factordb.py fetch
python3 tools/factordb.py submit
```

To stop a continuous run, create a `STOP` file in its work directory. The
worker finishes active jobs and submits their results. Remove the file before
resuming.

```sh
touch factordb-work/STOP
```

The work directory defaults to `factordb-work`. It stores queued inputs,
pending factors, and recently acknowledged inputs in `state.json`. The worker
checks the factor product and retains multiplicities before submission.
Transient RPC failures retry after `sleepseconds`. A pending result stays
saved until FactorDB acknowledges it.
