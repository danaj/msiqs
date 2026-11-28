# dmsiqs

Build with `make dmsiqs` on each machine. The host assigns polynomial
families and solves the matrix. Workers sieve and resolve their own
cofactors, including the full 3LP batch pipeline. `-t` selects sieve
threads on a worker and solver threads on the host.

Workers can be any type of host we support, including mixed architectures.
dmsiqs has successfully factored RSA-120 using a mix of AMD Zen5, Xeon,
Google Axion, and Apple Silicon workers.

## Worker source package

Run `make worker-tar` from the checkout. It produces
`dmsiqs-worker-1.tar.gz` for wire protocol version 1. Copy the archive to
each target machine and build there with a C compiler, GMP development
files, Make, and Perl. Python 3 is used for packaging and the coordinator.

```sh
tar -xzf dmsiqs-worker-1.tar.gz
cd dmsiqs-worker
make dmsiqs
./dmsiqs --version
```

The worker protocol version is the number sent in TCP frame headers.
`--version` also shows the Git revision and commit date when available.
The archive contains the current source files, and the Git revision in
its build stamp identifies the checkout's HEAD.

## Direct workers

Set `N` to the integer to factor. On each worker, start a listener with a
short name and the desired sieve thread count. For a trusted LAN, bind to
the worker's private interface and point the host at that address.

```sh
# On compute-b, set up a worker to use 48 threads
./dmsiqs --mode worker --bind "$WORKER_BIND" --name compute-b -t 48

# On the host, sieve with worker(s), up to 8 local solver threads
./dmsiqs --worker "$WORKER_BIND" -t 8 -v "$N"
```

A worker listens on `127.0.0.1:8012` by default. Starting one on the
factoring host and adding `--worker 127.0.0.1` uses it like any other
worker. A host holds each worker exclusively while connected and can
continue with its other workers after a disconnect. Each host run accepts
one integer. LP policy selection works as in `msiqs`, and cofactors below
270 bits use the local path.

## Coordinator discovery

For a pool that changes during a run, start the Python coordinator on a
trusted network. Set `COORDINATOR_ADDR` to its interface address with an
optional port, for example `192.168.1.10:8013`. Set `WORKER_BIND` to the
worker's local interface address.

```sh
# Coordinator
python3 tools/dmsiqs-coordinator.py --bind "$COORDINATOR_ADDR"

# On each worker, using a distinct name
./dmsiqs --mode worker --bind "$WORKER_BIND" --coord "$COORDINATOR_ADDR" --pool lab --name "$WORKER_NAME" -t 64

# Factoring host
./dmsiqs --coord "$COORDINATOR_ADDR" --pool lab -t 8 -vv "$N"
```

The coordinator and worker both accept `--bind ADDRESS[:PORT]`. The
coordinator defaults to `127.0.0.1:8013` and the worker to `127.0.0.1:8012`.
`--external`, `--coord`, and `--worker` also accept an address and optional
port. Bracket IPv6 addresses when specifying a port. `--coord` defaults to
port 8013 and `--worker` to 8012. With `--coord`, a worker advertises its
bind address and port by default. Set `--external` to the address the host
should dial when that differs from the bind address, for example behind
NAT or an SSH forward.
An external address without a port uses the bind port. A wildcard bind
requires `--external` when registering. Both listeners still accept a
separate `--port` for existing scripts. `dmsiqs` also accepts the older
`--coordinator` and `--advertise` aliases.

The default pool is `default`. Workers renew registrations about every
three seconds. The coordinator expires registrations after 12 seconds by
default and logs activation, address changes, and expiry. A worker with the
same name can register at a new address. The host polls for changes during
collection and waits when a coordinator pool is empty. Expiry or a
coordinator outage leaves existing worker connections running. Static
`--worker` entries can be used alongside discovery.

The [coordinator](tools/dmsiqs-coordinator.py) is a directory for worker
addresses. The host connects directly to each advertised endpoint. Both
protocols use plaintext TCP without peer authentication, and pool names
group workers for discovery. Use trusted private interfaces or SSH
forwarding for both paths.

## SSH forwarding (optional)

Keep the worker on its loopback listener. For a fixed worker, open a local
forward on the factoring host and point `--worker` at that local port.

```sh
# On compute-b
./dmsiqs --mode worker --name compute-b -t 48

# On the factoring host, in separate terminals
# 1. start the SSH tunnel
ssh -NT -o ExitOnForwardFailure=yes -L 127.0.0.1:18012:127.0.0.1:8012 compute-b
# 2. start the factoring job on the host using 8 threads
./dmsiqs --worker 127.0.0.1:18012 -t 8 -v "$N"
```

### Discovery over SSH (optional)

Run the coordinator on the factoring host. Open a local forward for
factoring traffic and a reverse forward so the worker can renew its lease.
The SSH server needs to permit both forwarding directions. Use the normal
SSH account and host-key verification for each worker.

```sh
# On the factoring host, in separate terminals
python3 tools/dmsiqs-coordinator.py
ssh -NT -o ExitOnForwardFailure=yes \
    -L 127.0.0.1:18012:127.0.0.1:8012 \
    -R 127.0.0.1:18013:127.0.0.1:8013 compute-b

# On compute-b
./dmsiqs --mode worker --name compute-b -t 48 --coord 127.0.0.1:18013 --external 127.0.0.1:18012 --pool ssh

# On the factoring host
./dmsiqs --coord 127.0.0.1 --pool ssh -t 8 -vv "$N"
```

The two loopback addresses on the worker command refer to different
machines. The worker dials `--coord 127.0.0.1:18013` on its own loopback.
It advertises `--external 127.0.0.1:18012` for the factoring host to dial
on the host's loopback. For each additional worker, choose a distinct
name and host-side port, and use that port in both `-L` and `--external`.
Separate worker machines can reuse ports 8012 and 18013. Keep the SSH
session open while the worker is in use.

On Compute Engine, pass the same forwarding options to `gcloud compute ssh`
after `--`. Use your normal VM SSH access and set `VM`, `GCE_PROJECT`, and
`ZONE` for the worker.

```sh
gcloud compute ssh "$VM" --project "$GCE_PROJECT" --zone "$ZONE" -- \
    -NT -o ExitOnForwardFailure=yes \
    -L 127.0.0.1:18012:127.0.0.1:8012 \
    -R 127.0.0.1:18013:127.0.0.1:8013
```

[OpenSSH forwarding](https://man.openbsd.org/ssh#L) · [Compute Engine SSH](https://docs.cloud.google.com/sdk/gcloud/reference/compute/ssh)

## Worker lifecycle

The host drops a worker with outstanding assignments if it reports neither
completed polynomials nor a nonempty relation batch for 600 seconds.
Use `--worker-stall SECONDS` to change this interval or `--worker-stall 0`
to disable it. The host logs the stall, waits at least 15 seconds before
retrying the endpoint, and reports it to the directory when configured.
The directory logs reports for all workers and removes stalled managed
endpoints from discovery until repaired, replaced, or expired.

The host retries temporary sieve setup failures at the same address with
backoff. Direct workers get two retries, and discovered workers can keep
retrying while listed. Each new connection negotiates claim capabilities,
including when a replacement worker uses an older claim format.

## Worker operations

Build an update in a fresh directory using the
[worker source package](#worker-source-package) and check the new binary
with `./dmsiqs --version`. Wait for the factoring host to finish or release
the worker before restarting it. Keep the worker's bind address, name,
thread count, and coordinator settings when restarting.

### Manually started workers

Stop the worker with Ctrl-C in its terminal, then run its startup command
from the new build directory. Workers write job events to stderr. For a
loopback worker, the following command appends those events to a file.

```sh
./dmsiqs --mode worker --name compute-b -t 48 2>>"$HOME/dmsiqs-worker.log"
```

Use `tail -F "$HOME/dmsiqs-worker.log"` in a separate terminal to follow the
worker log.

### Managed Axion workers

Pause the fleet watcher with Ctrl-C to avoid automated repair during
maintenance. On the VM,
log in as the configured worker user and set `NEW_WORKER` to the path of
the newly built executable. Install it at the service's existing path
and restart the service.

```sh
WORKER_DIR="$HOME/src/dmsiqs-worker"
install -m 755 "$NEW_WORKER" "$WORKER_DIR/dmsiqs.next" &&
    mv "$WORKER_DIR/dmsiqs.next" "$WORKER_DIR/dmsiqs" &&
    sudo systemctl restart dmsiqs-worker.service
"$WORKER_DIR/dmsiqs" --version
sudo journalctl -u dmsiqs-worker.service -n 100 --no-pager
```

If the transient unit has already unloaded, run
`sudo google_metadata_script_runner startup` to recreate it. Use
`sudo journalctl -u dmsiqs-worker.service -f` to follow live worker logs.
Resume the fleet watcher with `python3 tools/n4a-manager.py watch` on the
factoring network. Place any previous `--state` option before `watch`.

Replacement VMs use the snapshot recorded when the fleet was created.
For the next fleet, use `up --snapshot NAME` to select a snapshot built
with the updated worker. Record its name and `dmsiqs --version` output
with the run logs.

## Progress and accounting

With `-v`, the host shows the setup, timings, and results of collection and
solving. The distributed collection breakdown includes startup through the
first usable worker, time with usable workers, any later wait for capacity,
and the final drain. With `--journal`, `journal-info.py` also shows those
timings along with the matrix, solver, and dependency-test durations.

With `-vv`, the host lists its configured endpoints and coordinator pool
before dialing workers. Each worker setup line marks when the host queues
the setup, and the ready line gives the time from that point. The worker's
own job log gives its initialization time after it receives the setup.

One report tracks collection progress and worker state every minute by
default. It separates LP-free relations ready for the matrix from
accepted originals, which include smooth and partial relations after host
validation and deduplication. Worker rates show confirmed polys/s and
accepted rels/s. At `-vv`, a lone worker's row is omitted when it is
actively sieving and reported progress within the last second. At `-vvv`,
the row stays visible. Use `--worker-report 300` for five-minute reports
or `--worker-report 0` to keep aggregate progress and omit the worker rows.
Stalls and disconnects are reported when they happen. Long Lanczos runs show
elapsed time and iterations with `-vv`.

The final report includes each worker's share of confirmed polynomials,
accepted originals, core-hours, and peak RSS when available. The host
identifies the worker and its endpoint when it joins. Core-hours add user
and system CPU time and divide by 3,600. If a worker disconnects, the host
counts CPU through its last received batch and marks that job as
checkpoint-only. Totals flag incomplete CPU coverage. Outstanding work
charged to a disconnected worker appears separately from confirmed
polynomials. Worker peak RSS is a process-lifetime high-water mark and can
include earlier jobs.

With `--journal`, the host saves CPU totals with collection checkpoints and
at invocation end. The final report also gives journal-wide core-hours
after a restart. An interrupted invocation contributes its last saved
sample and is marked partial. The journal keeps the highest recorded host
peak RSS across invocations.

Use `-vvv` for exact wire and journal bytes, relation import time, queue
peaks, checkpoint ages, and the host's RSS high-water mark at matrix and
solver boundaries. A worker started with `-vvv` logs its own sieve-block
configuration.

Workers log the UTC start and completion or interruption of each job to
stderr, with elapsed time, CPU time, and work counts. Save host stdout and
worker stderr for per-worker history across host restarts.

## Journals and restart

Pass `--journal` to the factoring host to save accepted relations and
polynomial selection state. Resume with the same LP option and omit the
input to read it from the journal. Worker and coordinator addresses can
change between runs.

```sh
./dmsiqs --coord "$COORDINATOR_ADDR" --pool axion -t 8 --journal job.journal "$N"
./dmsiqs --coord "$COORDINATOR_ADDR" --pool axion -t 8 --journal job.journal
```

The host replays committed relations and continues choosing polynomial
families from the saved state. It drops outstanding worker assignments on
restart and rebuilds the matrix if the solve was interrupted. Worker
attribution covers the current invocation. The journal also reports total
core-hours across restarts. See the [journal guide](docs/JOURNAL.md) for
checkpoints, low-space warnings, and interrupted-write recovery.

## Using an Axion fleet -- a practical example

We include example scripts that can manage launching, monitoring, and removing
a fleet of Axion workers from Google Compute Engine.  If this is of interest,
you should be able to modify it to work with your cloud vendor or local setup,
including different kinds of hosts.

We chose N4A Axion for this example because it was inexpensive and our
Google Compute Engine project had enough quota to run six 64-core workers
across several zones.  In SIQS tests, C4D (AMD Zen 5 “Turin”) was roughly
twice the throughput per physical core, but our C4D quota didn't allow more.
The Axion fleet gave us a practical way to test launching, monitoring, and
replacing workers during a factoring run.

### Managing the worker pools

On the factoring network, run a coordinator with a two-minute lease and
start the local manager in a second terminal. The manager requires Python 3.10
or newer and an authenticated `gcloud`. Use a snapshot with the worker built
at `/home/VM_USER/src/dmsiqs-worker/dmsiqs` and allow TCP 8012 from the
factoring network. Service repair also requires noninteractive
`gcloud compute ssh` to the configured VM user and passwordless `sudo` on the VM. Set
`GCE_PROJECT` to your project ID and `N` to the input.

The GCE account needs `compute.zoneOperations.get` to monitor creates and
`compute.zoneOperations.list` to recover interrupted creates during `watch`
or `down`.
[Get a zonal operation](https://docs.cloud.google.com/compute/docs/reference/rest/v1/zoneOperations/get) ·
[List zonal operations](https://docs.cloud.google.com/compute/docs/reference/rest/v1/zoneOperations/list)

```sh
python3 tools/dmsiqs-coordinator.py --bind 127.0.0.1:8013 --ttl 120
python3 tools/n4a-manager.py up --project "$GCE_PROJECT" --count 6
./dmsiqs --coord 127.0.0.1:8013 --pool axion -t 8 -vv "$N"
```

`--count` accepts 1 through 12 and defaults to 6. The snapshot defaults to
`axion-snap`, and `--vm-user` defaults to the local username. The manager
checks each worker port every 30 seconds and renews the local registration if
it responds. Three consecutive misses or a stall report from the directory
trigger a GCE inventory lookup. The manager tries to restart the service on
a running VM or replaces a stopped VM in the next available zone in the launch
order. After a successful service restart, it tells the directory the endpoint
is ready for discovery again. The manager launches workers
in parallel across distinct zones, and `down` deletes the fleet in parallel.

Ctrl-C pauses management and leaves the VMs running. Run
`python3 tools/n4a-manager.py watch` to resume or
`python3 tools/n4a-manager.py down` to delete that fleet. Both use
`~/.dmsiqs/n4a.json` by default. Pass `--state` before the subcommand to
manage a fleet with a different state file.

The manager records each planned create in that state before submitting it.
After a timeout or interruption, `watch` checks for the same VM or its zonal
operation before launching another VM for that slot. `down` retains the state
while a create is unresolved or a delete fails. If it reports an unknown
outcome, use the logged VM name and zone to inspect the operation, then retry
`watch` or `down`:

```sh
gcloud compute operations list --project "$GCE_PROJECT" --zones "$ZONE" --filter "targetLink~'/instances/${VM_NAME}\$'" --format json
```

If the operation record has expired, confirm from GCE operations or audit logs
that the create cannot still complete and that the VM is gone. Then, with the
watcher stopped, remove the confirmed slot from `pending_creates` in the fleet
state and retry. An empty operation list alone leaves the create unresolved.
Existing fleet state still loads. If the previous manager was interrupted
during a launch, check that operation in GCE before restarting `watch` or
running `down`. The previous manager did not save pending creates.

### SSH integration for a managed fleet

The current manager probes and registers each VM's external IPv4 address
on TCP 8012. The startup script in [`n4a_gce.py`](tools/n4a_gce.py) binds
the worker to the VM's internal interface. Its SSH use is for service
operations. For manual forwarding to an existing service, use the VM's
actual bind address as the remote destination of `-L`. For workers set up
with the preceding SSH discovery example, use the loopback destination.

The [Axion manager](tools/n4a-manager.py) is the place to automate tunnels
if needed. It already knows the VM and worker slot, restarts the service,
and registers workers with the local directory. A tunnel-aware manager
can use a distinct loopback port per slot and open a local forward to
that slot's current VM. Check the worker on the remote side before
renewing the managed registration. A successful connection to the local
forward only establishes that the SSH client is listening there. Publish
the loopback endpoint in registration and repair messages, and keep the VM
address for service operations. Replace the tunnel when the VM changes
and close it when the fleet is shut down. The coordinator can use those
endpoints as ordinary addresses.

## Preliminary timings

Six `n4a-highcpu-64` instances (6 × 64 cores) performed collection. A MacBook M1 Pro
handled host coordination and linear algebra with eight threads.

| Input | Wall time | Core-hours |
| --- | ---: | ---: |
| RSA-100 | 16.4 s | 1.0 |
| RSA-110 | 1m 41.6s | 7.9 |
| RSA-120 | 10m 57.4s | 51.6 |
| RSA-130 | 86m 58.3s | 490.3 |

RSA-130 spent 9m 23s in Lanczos with eight threads. Core-hours include the
host and all workers. The AMD EPYC Turin cores in our single-machine runs
do more SIQS work per core-hour than the Axion cores used here.

## Checks

`make check-distributed` exercises the protocol, live discovery, solve retries,
host crash recovery, and complete forced-3LP factorizations with loopback
workers. The Python runner `tests/dmsiqs-check.py` also supports `--large` for
bounded RSA-110 and 499-bit collection checks and `--other-worker` for testing a
second worker build.

Run `python3 tests/n4a-manager-check.py` for the fleet-manager and coordinator
checks. They use offline test doubles for GCE and SSH.
