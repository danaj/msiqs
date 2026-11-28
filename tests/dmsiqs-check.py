#!/usr/bin/env python3
"""Exercise dmsiqs leases, framing, 3LP factorization, and journal recovery."""

import argparse
from contextlib import ExitStack, contextmanager, nullcontext
import errno
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


# The same known-factor 270-bit fixture as tests/journal/test-tlp.py.
N = 1148829108227602429702806949260709092933481257528975829355909861594747665058507777
P = 27062981639101571779863334701788273295629
Q = 42450204620755190797397639599411846697413

# Synthetic balanced 499-bit composite for setup and bounded collection.
LARGE_P = 1130782121458165970933310400475467850129589694000396133197827968827276669067
LARGE_Q = 1356938545749799165119972480570561420155507632800475359837393562592732042417
RSA_110 = 35794234179725868774991807832568455403003778024228226193532908190484670252364677411513516111204504060317568667

CLAIM, GRANTED, BUSY, SETUP, FAMILY, ERROR, PING, PONG = 1, 2, 3, 4, 6, 14, 15, 16
READY, DONE, RELATIONS, DIVISOR, PAUSE, PAUSED, END, ENDED = 5, 7, 8, 9, 10, 11, 12, 13
REGISTER, REGISTERED, LIST, LISTED = 17, 18, 19, 20
RELATIONS_CPU = 21
REPORT_STALL, REGISTER_MANAGED, STALLED, REPAIRED = 22, 23, 24, 25
CHECKPOINT_CLAIM = b"\x01"
CHECKPOINT_RSS_CLAIM = b"\x03"
SETUP_RETRY_ERROR = b"temporarily unable to start sieve pool"
HEADER = struct.Struct("!4sHHI")
CPU_USAGE = struct.Struct("!QQ")
CPU_RSS_USAGE = struct.Struct("!QQQ")
JOURNAL_V2 = b"MPUSIQJ\0" + struct.pack("<II", 2, 0x01020304)
JOURNAL_FRAME = struct.Struct("<4sIQIIQQQII")
JOURNAL_BATCH, JOURNAL_JOB_END, JOURNAL_SOLVE, JOURNAL_COLLECTION = 3, 6, 7, 8
JOURNAL_ACCOUNTING = 9
# BATCH progress stores the count of retained 3LP originals at u64 index 20.
JOURNAL_ORIGINALS_OFFSET = 20 * 8
ROOT = Path(__file__).resolve().parents[1]
COORDINATOR_PROGRAM = ROOT / "tools" / "dmsiqs-coordinator.py"


def read_exact(sock, length):
    pending = bytearray()
    while len(pending) < length:
        part = sock.recv(length - len(pending))
        if not part:
            raise EOFError("worker closed the connection")
        pending.extend(part)
    return bytes(pending)


def send(sock, kind, body=b"", version=1):
    sock.sendall(HEADER.pack(b"DMSQ", version, kind, len(body)) + body)


def receive(sock, deadline=None):
    original_timeout = sock.gettimeout()
    try:
        while True:
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("waiting for worker frame")
                sock.settimeout(remaining)
            magic, version, kind, length = HEADER.unpack(read_exact(sock, HEADER.size))
            maximum = (16 if kind == SETUP else 1) * 1024 * 1024
            assert magic == b"DMSQ" and version == 1 and length <= maximum
            body = read_exact(sock, length)
            if kind != PING:
                return kind, body
            assert not body
            send(sock, PONG)
    finally:
        if deadline is not None:
            sock.settimeout(original_timeout)


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


@contextmanager
def worker(executable, name, threads, *, log=None, registry=None, pool="default",
           bind=None, external=None, verbose=False):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    endpoint = ("127.0.0.1", port)
    with nullcontext(log) if log is not None else tempfile.TemporaryFile(mode="w+t") as errors:
        command = [str(executable), "--mode", "worker", "--name", name, "-t", str(threads)]
        if verbose:
            command.append("-vvv")
        command.extend(("--port", str(port)) if bind is None else ("--bind", f"{bind}:{port}"))
        if registry is not None:
            command.extend(("--pool", pool))
            if bind is None:
                command.extend(("--coordinator", registry, "--advertise", "127.0.0.1"))
            else:
                command.extend(("--coord", registry))
                if external is not None:
                    command.extend(("--external", external))
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            yield endpoint, process
        except Exception:
            errors.seek(0)
            print(errors.read()[-2400:], end="")
            raise
        finally:
            stop(process)


def claim(endpoint, process, *, fragmented=False, features=b""):
    deadline = time.monotonic() + 10
    while process.poll() is None and time.monotonic() < deadline:
        try:
            sock = socket.create_connection(endpoint, timeout=1)
        except ConnectionRefusedError:
            time.sleep(0.025)
            continue
        sock.settimeout(5)
        try:
            if fragmented:
                header = HEADER.pack(b"DMSQ", 1, CLAIM, len(features))
                sock.sendall(header[:3])
                time.sleep(0.025)
                sock.sendall(header[3:] + features)
            else:
                send(sock, CLAIM, features)
            kind, reply = receive(sock)
            if kind == GRANTED:
                return sock, reply
            assert (kind, reply) == (BUSY, b"")
        except BaseException:
            sock.close()
            raise
        sock.close()
        time.sleep(0.025)
    raise RuntimeError(f"worker {endpoint} failed to grant a lease")


def identity(body):
    fields = []
    for _ in range(4):
        length = body[0]
        assert 0 < length <= 127 and len(body) >= 1 + length
        fields.append(body[1:1 + length].decode("ascii", errors="replace"))
        body = body[1 + length:]
    return fields, struct.unpack("!I", body)[0]


def protocol(executable):
    with worker(executable, "check-lease", 2) as (endpoint, process):
        first, report = claim(endpoint, process, fragmented=True)
        with first:
            fields, threads = identity(report)
            assert fields[0] == "check-lease" and all(fields) and threads == 2
            with socket.create_connection(endpoint, timeout=5) as second:
                send(second, CLAIM, CHECKPOINT_RSS_CLAIM)
                assert receive(second) == (BUSY, b"")
            send(first, SETUP, b"\x00")
            assert receive(first) == (ERROR, b"invalid sieve setup")
        second, _ = claim(endpoint, process)
        with second:
            send(second, FAMILY)
            kind, error = receive(second)
            assert kind == ERROR and error
        # A version mismatch is rejected before either side reads the payload.
        third, _ = claim(endpoint, process)
        with third:
            send(third, SETUP, version=2)
            try:
                read_exact(third, 1)
            except (EOFError, ConnectionResetError):
                pass
            else:
                raise AssertionError("worker accepted the wrong protocol version")
        replacement, _ = claim(endpoint, process)
        replacement.close()
    print("PASS distributed protocol: identity, fragmented claim, exclusive lease, invalid setup and version, reacquire")


def mock_family(kind, body):
    assert kind == FAMILY and len(body) > 12 and len(body) % 4 == 0
    family_id, limit = struct.unpack_from("!QI", body)
    assert family_id > 0 and limit > 0
    return family_id, limit, body[12:]


def claim_mock(sock, name, deadline, features):
    sock.settimeout(20)
    assert receive(sock, deadline) == (CLAIM, features)
    fields = (name.encode("ascii"), b"loopback", b"mock", b"mock")
    identity_body = b"".join(bytes((len(field),)) + field for field in fields)
    send(sock, GRANTED, identity_body + struct.pack("!I", 1))
    kind, setup = receive(sock, deadline)
    assert kind == SETUP and setup
    return setup


def setup_mock(sock, name, timeout=20, *, features=CHECKPOINT_RSS_CLAIM):
    deadline = time.monotonic() + timeout
    setup = claim_mock(sock, name, deadline, features)
    send(sock, READY, struct.pack("!I", 2))
    assigned = [mock_family(*receive(sock, deadline)) for _ in range(2)]
    return setup, assigned


def finish_mock(sock, assigned, cpu=b""):
    divisor = P.to_bytes((P.bit_length() + 7) // 8, "big")
    send(sock, DIVISOR, bytes((len(divisor),)) + divisor)
    assert receive(sock) == (PAUSE, b"")
    for family_id, _, _ in assigned:
        send(sock, DONE, struct.pack("!QI", family_id, 1))
    send(sock, PAUSED)
    assert receive(sock) == (END, b"")
    send(sock, ENDED, cpu)


@contextmanager
def mock_listener():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)
        yield listener, f"127.0.0.1:{listener.getsockname()[1]}"


def wait_for_host_log(process, path, matches):
    deadline = time.monotonic() + 10
    while process.poll() is None and time.monotonic() < deadline:
        output = path.read_text()
        if matches(output):
            return
        time.sleep(0.05)
    raise AssertionError("host did not reach the expected state\n" + path.read_text()[-3000:])


@contextmanager
def coordinator(port=None, ttl=12, *, bind=None):
    command = [sys.executable, str(COORDINATOR_PROGRAM), "--ttl", str(ttl)]
    if bind is None and port is None:
        bind = "127.0.0.1:0"
    if bind is not None:
        command.extend(("--bind", bind))
    if port is not None:
        command.extend(("--port", str(port)))
    with subprocess.Popen(command, stderr=subprocess.PIPE, text=True) as process:
        try:
            assert select.select([process.stderr], [], [], 5)[0], "coordinator did not start"
            line = process.stderr.readline().strip()
            assert line.startswith("dmsiqs coordinator listening on "), line
            yield line.split()[-1], process
        finally:
            stop(process)


def registry_request(registry, kind, pool, *, name=None, endpoint=None):
    host, port = registry.rsplit(":", 1)
    pool = pool.encode("ascii")
    body = bytes((len(pool),)) + pool
    if name is not None:
        name = name.encode("ascii")
        body += bytes((len(name),)) + name
    if endpoint is not None:
        endpoint = endpoint.encode("ascii")
        body += struct.pack("!I", len(endpoint)) + endpoint
    with socket.create_connection((host.strip("[]"), int(port)), timeout=5) as sock:
        send(sock, kind, body)
        return receive(sock)


def wait_for_listing(registry, pool, listing):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if registry_request(registry, LIST, pool) == (LISTED, listing):
            return
        time.sleep(0.05)
    raise AssertionError("coordinator listing did not converge")


def check_coordinator_bind():
    cases = [(None, 0, "127.0.0.1"), ("127.0.0.1:0", 0, "127.0.0.1")]
    ipv6 = False
    try:
        with socket.socket(socket.AF_INET6) as probe:
            probe.bind(("::1", 0))
    except OSError:
        pass
    else:
        ipv6 = True
        cases.extend((("[::1]:0", None, "[::1]"), ("::1", 0, "[::1]")))
    for bind, port, address in cases:
        with coordinator(port=port, bind=bind) as (endpoint, _):
            host, published_port = endpoint.rsplit(":", 1)
            assert host == address and 1 <= int(published_port) <= 65535, endpoint
            assert registry_request(endpoint, LIST, "bind-check") == (LISTED, struct.pack("!I", 0))
    for arguments in (("--bind", "127.0.0.1:"),
                      ("--bind", "[::1"),
                      ("--bind", "127.0.0.1:65536"),
                      ("--bind", "127.0.0.1:0", "--port", "65535")):
        result = subprocess.run([sys.executable, str(COORDINATOR_PROGRAM), *arguments],
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 2, (arguments, result.returncode, result.stderr)
    coverage = " including bracketed and bare IPv6" if ipv6 else " (IPv6 loopback unavailable)"
    print("PASS coordinator bind: legacy port, duplicate ports, and invalid endpoints" + coverage)


def check_registration(executable):
    cases = (("axion", "127.0.0.1", None), ("external", "0.0.0.0", "127.0.0.1"),
             ("legacy", None, None))
    with coordinator(ttl=2) as (registry, _):
        for pool, bind, external in cases:
            with worker(executable, "check-registration", 1, registry=registry, pool=pool,
                        bind=bind, external=external) as (endpoint, _):
                address = f"{endpoint[0]}:{endpoint[1]}".encode("ascii")
                wait_for_listing(registry, pool, struct.pack("!II", 1, len(address)) + address)

        invalid = subprocess.run([str(executable), "--mode", "worker", "--coord", registry,
                                  "--bind", f"0.0.0.0:{endpoint[1]}"],
                                 text=True, capture_output=True, timeout=5)
        assert invalid.returncode == 2 and "--external" in invalid.stderr, invalid.stderr
        assert registry_request(registry, LIST, "other") == (LISTED, struct.pack("!I", 0))
        for advertised in ("127.0.0.1:5001", "127.0.0.1:5002"):
            assert registry_request(registry, REGISTER, "other", name="test", endpoint=advertised) == (REGISTERED, b"")
        assert registry_request(registry, LIST, "other") == (LISTED, struct.pack("!II", 1, len(advertised)) + advertised.encode("ascii"))
        for pool, _, _ in cases:
            wait_for_listing(registry, pool, struct.pack("!I", 0))
    print("PASS coordinator: real worker registration, pool isolation, and expiry")


def check_registry_lifecycle():
    first, second = "127.0.0.1:5001", "127.0.0.1:5002"
    with coordinator(ttl=1) as (registry, process):
        for endpoint in (first, first, second):
            assert registry_request(registry, REGISTER, "axion", name="worker", endpoint=endpoint) == (REGISTERED, b"")
        # Leave the registry idle. A LIST would itself expire old leases.
        time.sleep(2)
        stop(process)
        lines = process.stderr.read().splitlines()

    expected = (f"worker active in pool axion at {first}",
                f"worker moved in pool axion from {first} to {second}",
                f"worker lease expired in pool axion at {second}")
    assert len(lines) == len(expected), lines
    for line, message in zip(lines, expected):
        assert line.endswith(message), lines
    print("PASS coordinator: activation, address changes, quiet renewal, and idle expiry logs")


def check_discovery(executable):
    original = struct.pack("!IBBBI", 1, 1, 1, 0, 0)
    pool = "discover"
    waiting = f"waiting for workers in coordinator pool {pool}"
    with ExitStack() as stack:
        registry, broker = stack.enter_context(coordinator())
        first_listener, first_address = stack.enter_context(mock_listener())
        next_listener, next_address = stack.enter_context(mock_listener())
        directory = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="dmsiqs-discovery-")))
        log_path = directory / "host.log"
        log = stack.enter_context(log_path.open("w"))
        process = subprocess.Popen([str(executable), "--coord", registry, "--pool", pool,
                                    "-v", "--3lp", str(N)], stdout=log, stderr=subprocess.STDOUT)
        stack.callback(stop, process)
        try:
            wait_for_host_log(process, log_path, lambda output: waiting in output)
            assert registry_request(registry, REGISTER, pool, name="slot",
                                    endpoint=first_address) == (REGISTERED, b"")
            with first_listener.accept()[0] as first:
                setup, assigned = setup_mock(first, "check-first")
                send(first, RELATIONS, original)
                stop(broker)
                unavailable = f"coordinator {registry} unavailable"
                wait_for_host_log(process, log_path, lambda output: unavailable in output)
                send(first, DONE, struct.pack("!QI", assigned[0][0], 1))
                assigned.append(mock_family(*receive(first)))
            first_listener.close()
            # The same factoring host must wait again with no live workers.
            resumed, _ = stack.enter_context(coordinator(port=int(registry.split(":")[-1])))
            assert resumed == registry
            assert registry_request(registry, LIST, pool) == (LISTED, struct.pack("!I", 0))
            reachable = f"coordinator {registry} reachable"
            wait_for_host_log(process, log_path,
                              lambda output: output.count(waiting) >= 2 and reachable in output)
            assert registry_request(registry, REGISTER, pool, name="slot",
                                    endpoint=next_address) == (REGISTERED, b"")
            with next_listener.accept()[0] as second:
                setup_again, replacement = setup_mock(second, "check-second")
                assert setup_again == setup
                assert min(item[0] for item in replacement) > max(item[0] for item in assigned)
                assert not {item[2] for item in replacement} & {item[2] for item in assigned}
                send(second, RELATIONS, original)
                finish_mock(second, replacement)
            assert process.wait(timeout=20) == 0
            output = log_path.read_text()
            assert re.search(r"^# dmsiqs worker check-first: .*1 originals accepted", output, re.M)
            assert re.search(r"^# dmsiqs worker check-second: .*0/1 originals accepted/received", output, re.M)
            assert f"{N}: {P} {Q}" in output.splitlines()
        except Exception:
            stop(process)
            print(log_path.read_text()[-3000:], end="")
            raise
    print("PASS coordinator: late worker, work during outage, empty-pool wait, and replacement at a new address")


def check_reconnect(executable):
    # A valid smooth original with y=1 and an empty factor vector lets us
    # test the host's admission history without waiting to sieve a factor.
    trivial_original = struct.pack("!IBBBI", 1, 1, 1, 0, 0)
    with socket.socket() as listener, tempfile.TemporaryFile(mode="w+t") as log:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)
        endpoint = f"127.0.0.1:{listener.getsockname()[1]}"
        process = subprocess.Popen(
            [str(executable), "--worker", endpoint, "-v", "--3lp", str(N)],
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            first, _ = listener.accept()
            with first:
                setup, lost = setup_mock(first, "check-loss")
                for user_us in (900_000_000, 2_700_000_000):
                    send(first, RELATIONS_CPU, trivial_original + CPU_USAGE.pack(user_us, 900_000_000))
                # A truncated combined frame must leave the previous totals intact.
                body = trivial_original + CPU_USAGE.pack(5_400_000_000, 1_800_000_000)
                first.sendall(HEADER.pack(b"DMSQ", 1, RELATIONS_CPU, len(body)) + body[:-1])
            with listener.accept()[0] as old_worker:
                assert receive(old_worker, time.monotonic() + 20) == (CLAIM, CHECKPOINT_RSS_CLAIM)
                send(old_worker, ERROR, b"expected worker claim")
            with listener.accept()[0] as second:
                setup_again, replacement = setup_mock(second, "check-reconnect", features=CHECKPOINT_CLAIM)
                assert setup == setup_again
                assert min(item[0] for item in replacement) > max(item[0] for item in lost)
                assert not {item[2] for item in replacement} & {item[2] for item in lost}
                send(second, RELATIONS_CPU, trivial_original + CPU_USAGE.pack(3_600_000_000, 1_800_000_000))
                finish_mock(second, replacement, CPU_USAGE.pack(5_400_000_000, 1_800_000_000))
            assert process.wait(timeout=20) == 0
            log.seek(0)
            output = log.read()
            assert re.search(r"^# dmsiqs worker check-loss: .*1/2 originals accepted/received.*"
                             r"1\.0000 core-hours \(0/1 jobs reported, 1 checkpoint-only\)$", output, re.M)
            assert re.search(r"^# dmsiqs worker check-reconnect: .*0/1 originals accepted/received.*"
                             r"2\.0000 core-hours$", output, re.M)
            spent = sum(item[1] for item in lost)
            assert f"2 confirmed polys, {spent} unconfirmed polys charged after disconnect" in output
            assert re.search(r"^# dmsiqs CPU: 3\.0000 worker core-hours \(1/2 jobs reported, 1 checkpoint-only\).*partial total core-hours$", output, re.M)
            assert f"{N}: {P} {Q}" in output.splitlines()
        except Exception:
            log.seek(0)
            print(log.read()[-3000:], end="")
            raise
        finally:
            stop(process)
    print("PASS host reconnect: retains originals and CPU checkpoints, assigns fresh A families, settles pause")


def check_setup_retry(executable):
    with mock_listener() as (listener, endpoint), tempfile.TemporaryFile(mode="w+t") as log:
        process = subprocess.Popen([str(executable), "--worker", endpoint, "-v", "--3lp", str(N)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            with listener.accept()[0] as first:
                setup = claim_mock(first, "check-setup-retry", time.monotonic() + 20, CHECKPOINT_RSS_CLAIM)
                failed_at = time.monotonic()
                send(first, ERROR, SETUP_RETRY_ERROR)
            with listener.accept()[0] as second:
                assert time.monotonic() - failed_at >= 1.5, "setup retry did not back off"
                setup_again, assigned = setup_mock(second, "check-setup-retry")
                assert setup_again == setup
                finish_mock(second, assigned, CPU_RSS_USAGE.pack(0, 0, 0))
            assert process.wait(timeout=20) == 0
            log.seek(0)
            output = log.read()
            assert "could not start sieve pool; retrying setup" in output
            assert "(1/2 jobs reported)" in output
            assert f"{N}: {P} {Q}" in output.splitlines()
        except Exception:
            log.seek(0)
            print(log.read()[-3000:], end="")
            raise
        finally:
            stop(process)
    print("PASS temporary setup failure retries the same worker and completes with fresh assignments")


def check_legacy_replacement(executable):
    with mock_listener() as (listener, endpoint), tempfile.TemporaryFile(mode="w+t") as log:
        process = subprocess.Popen([str(executable), "--worker", endpoint, "--3lp", str(N)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            with listener.accept()[0] as first:
                setup, lost = setup_mock(first, "check-modern")
            # A legacy replacement may close on unknown claim contents without
            # sending an explicit error. Fall back even after a prior lease.
            for features in (CHECKPOINT_RSS_CLAIM, CHECKPOINT_CLAIM):
                with listener.accept()[0] as peer:
                    assert receive(peer, time.monotonic() + 20) == (CLAIM, features)
            with listener.accept()[0] as legacy:
                setup_again, assigned = setup_mock(legacy, "check-legacy", features=b"")
                assert setup_again == setup
                assert min(item[0] for item in assigned) > max(item[0] for item in lost)
                finish_mock(legacy, assigned)
            assert process.wait(timeout=20) == 0
            log.seek(0)
            assert f"{N}: {P} {Q}" in log.read().splitlines()
        except Exception:
            log.seek(0)
            print(log.read()[-3000:], end="")
            raise
        finally:
            stop(process)
    print("PASS legacy worker replacement renegotiates a silent claim close at the same address")


def check_worker_stall(executable):
    original = struct.pack("!IBBBI", 1, 1, 1, 0, 0)
    pool = "stall"
    with ExitStack() as stack:
        registry, _ = stack.enter_context(coordinator(ttl=60))
        first_listener, first_address = stack.enter_context(mock_listener())
        next_listener, next_address = stack.enter_context(mock_listener())
        next_listener.settimeout(8)
        directory = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="dmsiqs-stall-")))
        log_path = directory / "host.log"
        log = stack.enter_context(log_path.open("w"))
        assert registry_request(registry, REGISTER_MANAGED, pool, name="slot",
                                endpoint=first_address) == (REGISTERED, b"")
        command = [str(executable), "--coord", registry, "--pool", pool, "--worker-stall", "2",
                   "--worker", first_address, "-v", "--3lp", str(N)]
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        stack.callback(stop, process)
        try:
            with first_listener.accept()[0] as first:
                setup, lost = setup_mock(first, "check-stalled")
                send(first, RELATIONS_CPU, original + CPU_USAGE.pack(2_700_000_000, 900_000_000))
                # Keep the connection responsive while doing no work on outstanding families.
                deadline = time.monotonic() + 10
                replies = 0
                while time.monotonic() < deadline:
                    try:
                        send(first, PING)
                        assert receive(first, deadline) == (PONG, b"")
                        send(first, PONG)
                    except (EOFError, BrokenPipeError, ConnectionResetError):
                        break
                    replies += 1
                    time.sleep(0.1)
                else:
                    raise AssertionError("host kept the stalled worker connected")
                assert replies > 0, "worker disconnected before completing a heartbeat"
            wait_for_host_log(process, log_path, lambda output: "made no sieve progress" in output)
            wait_for_listing(registry, pool, struct.pack("!I", 0))
            assert registry_request(registry, REGISTER_MANAGED, pool, name="slot",
                                    endpoint=first_address) == (STALLED, b"")
            # The old address was also specified directly. Skip its normal two-second retry.
            assert not select.select([first_listener], [], [], 3)[0], "host redialed the stalled worker"
            assert process.poll() is None
            assert registry_request(registry, REPAIRED, pool, endpoint=first_address) == (REGISTERED, b"")
            assert registry_request(registry, REGISTER_MANAGED, pool, name="slot",
                                    endpoint=first_address) == (REGISTERED, b"")
            assert registry_request(registry, REGISTER_MANAGED, pool, name="slot",
                                    endpoint=next_address) == (REGISTERED, b"")
            assert registry_request(registry, REPORT_STALL, pool, endpoint=first_address) == (REGISTERED, b"")
            with next_listener.accept()[0] as second:
                setup_again, replacement = setup_mock(second, "check-unstalled")
                assert setup_again == setup
                assert min(item[0] for item in replacement) > max(item[0] for item in lost)
                assert not {item[2] for item in replacement} & {item[2] for item in lost}
                send(second, RELATIONS_CPU, original + CPU_USAGE.pack(3_600_000_000, 1_800_000_000))
                finish_mock(second, replacement, CPU_RSS_USAGE.pack(5_400_000_000, 1_800_000_000, 5 << 19))
            assert process.wait(timeout=20) == 0
            output = log_path.read_text()
            assert re.search(r"^# dmsiqs worker check-stalled: .*1 originals accepted.*"
                             r"1\.0000 core-hours \(0/1 jobs reported, 1 checkpoint-only\)$", output, re.M)
            assert re.search(r"^# dmsiqs worker check-unstalled: .*0/1 originals accepted/received.*"
                             r"2\.0000 core-hours, peak RSS 2\.5 MiB$", output, re.M)
            spent = sum(item[1] for item in lost)
            assert f"2 confirmed polys, {spent} unconfirmed polys charged after disconnect" in output
            assert f"{N}: {P} {Q}" in output.splitlines()
        except Exception:
            stop(process)
            print(log_path.read_text()[-3000:], end="")
            raise
    print("PASS worker stall: live heartbeat, directory notice, repair clearance, and fresh work at a new address")


def reject_bad_batch(executable):
    # The y=1 record satisfies the congruence. The y=2 record does not. A
    # transport batch must pass complete validation before admission.
    batch = struct.pack("!IBBBIBBBI", 2, 1, 1, 0, 0, 1, 2, 0, 0)
    with socket.socket() as listener, tempfile.TemporaryFile(mode="w+t") as log:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)
        endpoint = f"127.0.0.1:{listener.getsockname()[1]}"
        process = subprocess.Popen(
            [str(executable), "--worker", endpoint, "-v", "--3lp", str(N)],
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            for features in (CHECKPOINT_RSS_CLAIM, CHECKPOINT_CLAIM):
                with listener.accept()[0] as first:
                    assert receive(first, time.monotonic() + 20) == (CLAIM, features)
                    # Workers predating feature flags close on a nonempty claim.
            peer, _ = listener.accept()
            with peer:
                setup_mock(peer, "check-bad-batch", features=b"")
                send(peer, RELATIONS, batch)
                assert process.wait(timeout=20) != 0
            log.seek(0)
            output = log.read()
            assert "sent an invalid frame" in output
            assert re.search(r"^# dmsiqs worker check-bad-batch: .*0 originals accepted", output, re.M)
            assert "[incomplete]" in output
        except Exception:
            log.seek(0)
            print(log.read()[-3000:], end="")
            raise
        finally:
            stop(process)
    print("PASS legacy claim fallback and host relation validation: a bad congruence rejects the complete batch")


def integration(executable, other_worker, timeout):
    identities = {}
    with ExitStack() as stack:
        registry, _ = stack.enter_context(coordinator())
        command = [str(executable), "--3lp", "-vvv", "-t", "2", "--coordinator", registry]
        for name, binary in (("check-alpha", executable), ("check-beta", other_worker)):
            discover = name == "check-alpha"
            endpoint, process = stack.enter_context(worker(binary, name, 2, registry=registry if discover else None))
            ready, hello = claim(endpoint, process)
            ready.close()
            fields, threads = identity(hello)
            assert fields[0] == name and threads == 2
            identities[name] = fields[1:]
            if not discover:
                command.extend(("--worker", f"{endpoint[0]}:{endpoint[1]}"))
        command.append(str(N))
        result = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    assert result.returncode == 0, result.stderr[-3000:] + result.stdout[-3000:]
    assert P * Q == N
    lines = result.stdout.splitlines()
    assert f"{N}: {P} {Q}" in lines, result.stdout[-3000:]
    static = command[command.index("--worker") + 1]
    assert f"# dmsiqs configured workers: {static}" in lines
    assert f"# dmsiqs discovering workers at {registry}, pool default" in lines
    if sys.platform in ("darwin", "linux"):
        phases = re.findall(r"^# dmsiqs host peak RSS (.+): ([\d.]+) MiB \(process high-water\)$",
                            result.stdout, re.M)
        assert [phase for phase, _ in phases[:3]] == [
            "before matrix construction", "after matrix reduction", "after Lanczos"], result.stdout[-3000:]
        peaks = [float(value) for _, value in phases]
        assert peaks[0] > 0 and peaks == sorted(peaks), peaks
    shares = []
    accounted = []
    for name, (hostname, system, machine) in identities.items():
        worker_prefix = f"# dmsiqs worker {name} ("
        beginning = next(line for line in lines if line.startswith(worker_prefix) and
                         " starting setup at " in line)
        ready = next(line for line in lines if line.startswith(worker_prefix) and " ready: " in line)
        assert beginning.endswith(" bytes queued)") and ", setup " in ready
        assert lines.index(beginning) < lines.index(ready)
        prefix = f"# dmsiqs worker {name} ({hostname}, {system} {machine}, 2 threads, "
        report = next((line for line in result.stdout.splitlines() if line.startswith(prefix)), "")
        totals = re.search(r"(\d+) families with confirmed work, (\d+) confirmed polynomials "
                           r"\(([\d.]+)%\), (\d+)/(\d+) originals accepted/received", report)
        assert totals and all(float(value) > 0 for value in totals.groups()), result.stdout[-3000:]
        assert re.search(r", peak receive queue [1-9]\d* bytes", report), report
        shares.append(float(totals[3]))
        cpu = re.search(r", ([\d.]+) core-hours \(1/1 jobs reported\)(?:, peak RSS ([\d.]+) MiB)?$", report)
        if name == "check-alpha" or other_worker == executable:
            assert cpu and float(cpu[1]) > 0, result.stdout[-3000:]
            assert cpu[2] and float(cpu[2]) > 0, result.stdout[-3000:]
        if cpu:
            accounted.append(float(cpu[1]))
    assert abs(sum(shares) - 100.0) <= 0.1, shares
    cpu = re.search(r"^# dmsiqs CPU: ([\d.]+) worker core-hours \((\d+)/2 jobs reported\), ([\d.]+) host core-hours, ([\d.]+) (partial )?total core-hours$", result.stdout, re.M)
    assert cpu and int(cpu[2]) == len(accounted) and bool(cpu[5]) == (len(accounted) < 2), result.stdout[-3000:]
    assert abs(float(cpu[1]) - sum(accounted)) <= 0.000002, result.stdout[-3000:]
    assert float(cpu[3]) > 0 and abs(float(cpu[1]) + float(cpu[3]) - float(cpu[4])) <= 0.000002
    progress = [line for line in result.stdout.splitlines() if line.startswith("# siqs relations ")]
    assert progress and all(", pending " not in line for line in progress), result.stdout[-3000:]
    assert re.search(r"^# siqs accepted .* [1-9]\d* three-LP$", result.stdout, re.M), result.stdout[-3000:]
    assert "# siqs 3LP batches:" not in result.stdout, result.stdout[-3000:]
    machines = " + ".join(fields[2] for fields in identities.values())
    print(f"PASS distributed integration: complete forced-3LP factorization with one discovered and one static worker ({machines})")


def journal_frames(data):
    """Return complete CRC-verified v2 frames, including their byte ranges."""
    delimiter = data.find(b"\n--\n")
    binary = delimiter + 4
    offset = binary + len(JOURNAL_V2)
    if delimiter < 0 or offset > len(data):
        return []
    assert data[binary:offset] == JOURNAL_V2
    frames = []
    while offset + JOURNAL_FRAME.size <= len(data):
        magic, kind, sequence, size, _, _, _, _, header_crc, crc = JOURNAL_FRAME.unpack_from(data, offset)
        assert magic == b"JBLK" and sequence == len(frames)
        assert header_crc == zlib.crc32(data[offset:offset + 48])
        payload = offset + JOURNAL_FRAME.size
        end = payload + size
        if end > len(data):
            break
        assert crc == zlib.crc32(data[payload:end], zlib.crc32(data[offset:offset + 52]))
        frames.append((kind, payload, end))
        offset = end
    return frames


def reject_initial_worker(command, journal):
    with socket.socket() as listener, tempfile.TemporaryFile(mode="w+t") as log:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)
        address = f"127.0.0.1:{listener.getsockname()[1]}"
        host = subprocess.Popen(command + ["--worker", address, str(N)],
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            with listener.accept()[0] as peer:
                assert receive(peer, time.monotonic() + 20) == (CLAIM, CHECKPOINT_RSS_CLAIM)
                send(peer, ERROR, b"initial worker rejected this test job")
                status = host.wait(timeout=20)
            log.seek(0)
            output = log.read()
            assert status != 0 and "no compatible workers available" in output, output[-3000:]
        finally:
            stop(host)
    saved = journal.read_bytes()
    frames = journal_frames(saved)
    assert [kind for kind, _, _ in frames] == [1, 2, JOURNAL_COLLECTION, JOURNAL_ACCOUNTING]
    assert frames[-1][2] == len(saved), "initial worker rejection should leave an open attempt"
    return saved


def interrupt_collection(command, journal, log_path, timeout):
    with log_path.open("w") as log:
        host = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            while host.poll() is None and time.monotonic() < deadline:
                if journal.exists():
                    data = journal.read_bytes()
                    frames = journal_frames(data)
                    if any(kind == JOURNAL_BATCH and
                           struct.unpack_from("<Q", data, payload + JOURNAL_ORIGINALS_OFFSET)[0] and
                           next_kind == JOURNAL_ACCOUNTING
                           for (kind, payload, _), (next_kind, _, _) in zip(frames, frames[1:])):
                        break
                time.sleep(0.05)
            else:
                raise AssertionError("no committed relation batch before interruption\n" +
                                     log_path.read_text()[-3000:])
            host.kill()
            assert host.wait(timeout=10) == -signal.SIGKILL
        finally:
            stop(host)
    interrupted = journal.read_bytes()
    frames = journal_frames(interrupted)
    assert frames and frames[-1][0] in (JOURNAL_BATCH, JOURNAL_ACCOUNTING), \
        "host finished collection before interruption"
    _, payload, _ = next(frame for frame in reversed(frames) if frame[0] == JOURNAL_BATCH)
    end = frames[-1][2]
    families, = struct.unpack_from("<Q", interrupted, payload)
    originals, = struct.unpack_from("<Q", interrupted, payload + JOURNAL_ORIGINALS_OFFSET)
    assert families > 0 and originals > 0
    committed = interrupted[:end]
    if len(committed) == len(interrupted):
        # If the real kill left no short frame, simulate a torn header.
        with journal.open("ab") as file:
            file.write(b"JBLK")
    return committed, families, originals


def resume_with_replacement(command, log_path, registry, pool, name, address, timeout):
    with log_path.open("w") as log:
        host = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            while host.poll() is None and time.monotonic() < deadline:
                output = log_path.read_text()
                if (f"# dmsiqs waiting for workers in coordinator pool {pool}" in output and
                        re.search(r"^#   journal checkpoint .+ ago$", output, re.M)):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("resumed host did not report its checkpoint while waiting\n" +
                                     log_path.read_text()[-3000:])
            assert registry_request(registry, REGISTER, pool, name=name,
                                    endpoint=address) == (REGISTERED, b"")
            assert host.wait(timeout=timeout) == 0, log_path.read_text()[-3000:]
        finally:
            stop(host)
    output = log_path.read_text()
    assert f"worker {name} ({address}) ready" in output, output[-3000:]
    return output


def check_journal_accounting(data, frames, output):
    snapshots = {}
    for kind, payload, end in frames:
        if kind == JOURNAL_ACCOUNTING:
            run = JOURNAL_FRAME.unpack_from(data, payload - JOURNAL_FRAME.size)[5]
            snapshots[run] = struct.unpack("<8QI", data[payload:end])
    assert sorted(snapshots) == [0, 1, 2], snapshots
    assert [snapshot[8] & 2 for snapshot in snapshots.values()] == [2, 0, 2]
    assert all(sum(snapshots[run][:2]) > 0 for run in (1, 2))
    worker_hours = sum(sum(snapshot[:2]) for snapshot in snapshots.values()) / 3_600_000_000
    host_hours = sum(sum(snapshot[2:4]) for snapshot in snapshots.values()) / 3_600_000_000
    cpu = re.search(r"^# dmsiqs journal CPU: ([\d.]+) worker core-hours .*?, "
                    r"([\d.]+) host core-hours, ([\d.]+) partial total core-hours "
                    r"\(3/3 invocations recorded, 2 final\)$", output, re.M)
    assert cpu, output[-3000:]
    for value, expected in zip(cpu.groups(), (worker_hours, host_hours, worker_hours + host_hours)):
        assert abs(float(value) - expected) <= 0.000001
    if sys.platform in ("darwin", "linux"):
        peak = max(snapshot[4] for snapshot in snapshots.values())
        assert peak > 0 and any(f"# dmsiqs {label}host peak RSS: {peak / 1024**2:.1f} MiB" in output
                                for label in ("", "journal "))


def check_journal_disk_full(executable, fault_executable, journal, prefix, endpoint, timeout):
    journal.write_bytes(prefix)
    options = ["-v", "-t", "2", "--3lp", "--journal", str(journal),
               "--worker", f"{endpoint[0]}:{endpoint[1]}"]
    factorization = f"{N}: {P} {Q}"
    cases = [
        ("batch-header", JOURNAL_BATCH, 11, errno.EFBIG),
        ("batch-payload", JOURNAL_BATCH, JOURNAL_FRAME.size + 19, errno.ENOSPC),
        ("accounting-payload", JOURNAL_ACCOUNTING, JOURNAL_FRAME.size + 19, errno.ENOSPC),
    ]
    for mode, interrupted_kind, tail_size, error in cases:
        previous = journal_frames(prefix)
        env = dict(os.environ, DMSIQS_TEST_JOURNAL_WRITE_FAULT=mode)
        result = subprocess.run([str(fault_executable), *options], env=env,
                                capture_output=True, text=True, timeout=timeout)
        assert result.returncode == 3 and os.strerror(error) in result.stderr, \
            result.stderr[-3000:] + result.stdout[-3000:]
        if error == errno.ENOSPC:
            assert "Free space and resume with this journal" in result.stderr, result.stderr
        assert factorization not in result.stdout.splitlines(), result.stdout[-3000:]
        data = journal.read_bytes()
        assert data.startswith(prefix), "write failure changed committed bytes"
        frames = journal_frames(data)
        if interrupted_kind == JOURNAL_ACCOUNTING:
            assert frames[:-1] == previous and frames[-1][0] == JOURNAL_BATCH
        else:
            assert frames == previous
        complete_end = frames[-1][2]
        assert len(data) == complete_end + tail_size, (mode, len(data) - complete_end)
        assert struct.unpack_from("<4sI", data, complete_end) == (b"JBLK", interrupted_kind)
        prefix = data[:complete_end]
        print(f"PASS distributed journal {mode}: preserved committed frames after {errno.errorcode[error]}")

    _, payload, _ = next(frame for frame in reversed(frames) if frame[0] == JOURNAL_BATCH)
    families, = struct.unpack_from("<Q", prefix, payload)
    originals, = struct.unpack_from("<Q", prefix, payload + JOURNAL_ORIGINALS_OFFSET)
    replayed = f"# siqs journal replayed {originals} accepted relations, {families} A families; appending new work"
    env = dict(os.environ, SIQS_TEST_JOURNAL_LOW_SPACE="1")
    result = subprocess.run([str(fault_executable), *options], env=env,
                            capture_output=True, text=True, timeout=timeout)
    assert result.returncode == 0 and factorization in result.stdout.splitlines(), \
        result.stderr[-3000:] + result.stdout[-3000:]
    assert "ignoring incomplete final frame" in result.stderr and replayed in result.stdout, \
        result.stderr[-3000:] + result.stdout[-3000:]
    warning = f"SIQS journal {journal}: low disk space, 0.0 MiB free"
    assert result.stderr.count(warning) == 1, result.stderr[-3000:]
    completed = journal.read_bytes()
    assert completed.startswith(prefix)
    kind, _, end = journal_frames(completed)[-1]
    assert kind == JOURNAL_JOB_END and end == len(completed)
    replay = subprocess.run([str(executable), *options], capture_output=True, text=True, timeout=timeout)
    assert replay.returncode == 0 and factorization in replay.stdout.splitlines(), \
        replay.stderr[-3000:] + replay.stdout[-3000:]
    assert journal.read_bytes() == completed
    print("PASS distributed journal disk-full recovery: resumed from committed batch and verified factors")
    print("PASS distributed journal space warning: reported once and continued to completion")


def recovery(executable, timeout, fault_executable=None):
    pool = "recovery"
    name = "check-recovery"
    factorization = f"{N}: {P} {Q}"
    with tempfile.TemporaryDirectory(prefix="dmsiqs-recovery-") as directory:
        journal = Path(directory) / "run.journal"
        first_log = Path(directory) / "first.log"
        resumed_log = Path(directory) / "resumed.log"
        base_command = [str(executable), "-t", "2", "-vvv", "--worker-report", "1",
                        "--3lp", "--journal", str(journal)]
        empty_attempt = reject_initial_worker(base_command, journal)
        with worker(executable, name, 1) as (first_endpoint, first_worker):
            with coordinator(ttl=2 * timeout) as (registry, _):
                ready, _ = claim(first_endpoint, first_worker)
                ready.close()
                first_address = f"{first_endpoint[0]}:{first_endpoint[1]}"
                assert registry_request(registry, REGISTER, pool, name=name,
                                        endpoint=first_address) == (REGISTERED, b"")
                command = base_command + ["--coord", registry, "--pool"]
                committed, families, originals = interrupt_collection(
                    command + [pool], journal, first_log, timeout)
                assert committed.startswith(empty_attempt), "resume rewrote the initial context"

            # Bind the replacement before stopping the original, so its port differs.
            with worker(executable, name, 4) as (next_endpoint, next_worker):
                assert next_endpoint != first_endpoint
                stop(first_worker)
                ready, _ = claim(next_endpoint, next_worker)
                ready.close()
                next_address = f"{next_endpoint[0]}:{next_endpoint[1]}"
                with coordinator(port=int(registry.split(":")[-1]), ttl=2 * timeout) as (resumed_registry, _):
                    assert resumed_registry == registry
                    output = resume_with_replacement(command + [pool], resumed_log, registry, pool,
                                                     name, next_address, timeout)
                    replayed = (f"# siqs journal replayed {originals} accepted relations, "
                                f"{families} A families; appending new work")
                    assert replayed in output.splitlines(), output[-3000:]
                    assert P * Q == N and factorization in output.splitlines(), output[-3000:]
                    completed = journal.read_bytes()
                    assert completed.startswith(committed), "resume rewrote committed bytes"
                    frames = journal_frames(completed)
                    kind, _, end = frames[-1]
                    assert kind == JOURNAL_JOB_END and end == len(completed), "resume did not seal the journal"
                    check_journal_accounting(completed, frames, output)
                    collections = [struct.unpack("<4Q", completed[payload:end])
                                   for kind, payload, end in frames if kind == JOURNAL_COLLECTION]
                    assert len(collections) >= 2 and "# dmsiqs phases:" in output
                    journal_growth = [(int(delta), int(total)) for delta, total in re.findall(
                        r"^# dmsiqs collection resources: .*?, \+(\d+) journal bytes \((\d+) total\)$", output, re.M)]
                    collection_ends = [end for kind, _, end in frames
                                       if kind == JOURNAL_COLLECTION and end > len(committed)]
                    assert [total for _, total in journal_growth] == collection_ends
                    assert all(0 < delta <= total - len(committed) for delta, total in journal_growth)
                    assert collections[0][1:3] == (0, 0), "initial rejection had no usable worker"
                    startup_us, collection_us, _, drain_us = collections[1]
                    assert startup_us >= 1_000_000 and collection_us > 0 and drain_us > 0
                    solves = [struct.unpack("<7Q6I", completed[payload:end])
                              for kind, payload, end in frames if kind == JOURNAL_SOLVE]
                    assert solves, "missing solve summary"
                    (rows, columns, entries, _, solver_us, _, iterations,
                     method, threads, attempts, returned, tested, found) = solves[-1]
                    assert 0 < rows < columns and entries > 0 and solver_us > 0
                    assert method in (2, 3) and iterations > 0 and 1 <= threads <= 2 and attempts > 0
                    assert 1 <= tested <= returned <= 64 and found == 1
                    replay = subprocess.run(command + ["recovery-empty"],
                                            capture_output=True, text=True, timeout=timeout)
                    assert replay.returncode == 0 and factorization in replay.stdout.splitlines(), \
                        replay.stderr[-3000:] + replay.stdout[-3000:]
                    check_journal_accounting(completed, frames, replay.stdout)
                    assert journal.read_bytes() == completed, "completed replay rewrote journal"
                if fault_executable is not None:
                    check_journal_disk_full(executable, fault_executable, Path(directory) / "disk-full.journal",
                                            committed, next_endpoint, timeout)
    print("PASS distributed recovery: initial worker rejection, 3LP host kill, torn-tail replay, new-address worker, and verified factors")


def bounded_setup(executable, n, bits, *, force_3lp=False):
    assert n.bit_length() == bits
    # Capture a regular host's authoritative setup and one SAR assignment.
    # A raw connection to a real worker can then cap the polynomial budget
    # and exercise collection and drain without attempting a solve.
    with socket.socket() as listener, tempfile.TemporaryFile(mode="w+t") as log:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(120)
        endpoint = f"127.0.0.1:{listener.getsockname()[1]}"
        command = [str(executable), "--worker", endpoint, str(n)]
        if force_3lp:
            command.insert(1, "--3lp")
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            capture, _ = listener.accept()
            with capture:
                setup, assignments = setup_mock(capture, "capture-large", timeout=120)
                assert int.from_bytes(setup[1:1 + setup[0]], "big") == n
                # Setup starts with N, k, then FB size, M, q, d, and LP count.
                assert struct.unpack_from("!6I", setup, 1 + setup[0])[-1] == 3
        except Exception:
            stop(process)
            log.seek(0)
            print(log.read()[-3000:], end="")
            raise
        finally:
            stop(process)

    with tempfile.TemporaryFile(mode="w+t") as log, worker(executable, "check-bounded", 1,
                                                           log=log, verbose=force_3lp) as (endpoint, process):
        real, _ = claim(endpoint, process, features=CHECKPOINT_RSS_CLAIM)
        with real:
            send(real, SETUP, setup)
            assert receive(real, time.monotonic() + 120) == (READY, struct.pack("!I", 2))
            family_id, limit, indices = assignments[0]
            budget = min(limit, 128)
            started = time.monotonic()
            send(real, FAMILY, struct.pack("!QI", family_id, budget) + indices)
            records = 0
            checkpoints = []
            for end_kind, end_body in ((DONE, struct.pack("!QI", family_id, budget)), (PAUSED, b"")):
                deadline = time.monotonic() + 60
                while True:
                    kind, body = receive(real, deadline)
                    if kind == end_kind:
                        assert body == end_body
                        break
                    assert kind == RELATIONS_CPU, (kind, body[:200])
                    checkpoints.append(CPU_USAGE.unpack_from(body, len(body) - CPU_USAGE.size))
                    count, = struct.unpack_from("!I", body)
                    assert count > 0
                    records += count
                if end_kind == DONE:
                    send(real, PAUSE)
            if force_3lp:
                assert checkpoints, "bounded 3LP collection returned no relation batches"
            send(real, END)
            kind, body = receive(real, time.monotonic() + 60)
            assert kind == ENDED
            user, system, peak_rss = CPU_RSS_USAGE.unpack(body)
            assert peak_rss > 0
            final = (user, system)
            assert sum(final) > 0
            for earlier, later in zip(checkpoints, checkpoints[1:] + [final]):
                assert all(a <= b for a, b in zip(earlier, later)), (earlier, later)
            elapsed = time.monotonic() - started
            if force_3lp:
                send(real, SETUP, setup)
                assert receive(real, time.monotonic() + 60) == (READY, struct.pack("!I", 2))
        if force_3lp:
            replacement, _ = claim(endpoint, process, features=CHECKPOINT_CLAIM)
            with replacement:
                deadline = time.monotonic() + 60
                send(replacement, SETUP, setup)
                assert receive(replacement, deadline) == (READY, struct.pack("!I", 2))
                send(replacement, PAUSE)
                assert receive(replacement, deadline) == (PAUSED, b"")
                send(replacement, END)
                kind, body = receive(replacement, deadline)
                assert kind == ENDED and len(body) == CPU_USAGE.size
        log.seek(0)
        output = log.read()
        for event in ("Started", "Completed", "Interrupted") if force_3lp else ("Started", "Completed"):
            assert re.search(rf"^\d{{4}}-\d\d-\d\dT\d\d:\d\d:\d\dZ.*{event} job", output, re.M), output[-2500:]
        assert re.search(r"Started job on worker check-bounded .*?, setup [^)\n]+\)$", output, re.M), output[-2500:]
        assert ("Worker check-bounded sieve block" in output) == force_3lp, output[-2500:]
        assert "core-hours" in output, output[-2500:]
    print(f"PASS {bits}-bit 3LP, {len(setup):,} setup bytes, {budget} polynomials and {records} originals in {elapsed:.2f}s with worker CPU accounting")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "dmsiqs")
    parser.add_argument("--other-worker", type=Path, help="use a second binary for one integration worker")
    parser.add_argument("--write-fault-executable", type=Path, help="exercise journal short writes and ENOSPC")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--quick", action="store_true", help="skip full factorizations")
    mode.add_argument("--recovery-only", action="store_true", help="run only the distributed journal recovery test")
    parser.add_argument("--large", action="store_true", help="also check bounded RSA-110 and 499-bit worker collection")
    parser.add_argument("--timeout", type=int, default=600, help="timeout for the full solve in seconds")
    args = parser.parse_args()
    executable = args.executable.resolve()
    version = subprocess.run([str(executable), "--version"], text=True, capture_output=True,
                             timeout=5, check=True)
    assert re.fullmatch(r"dmsiqs version 1(?:, git [0-9a-f]{8} \d{1,2} [A-Z][a-z]{2} \d{4})?\n",
                        version.stdout), version.stdout
    fault_executable = args.write_fault_executable.resolve() if args.write_fault_executable else None
    if args.recovery_only:
        recovery(executable, args.timeout, fault_executable)
        return
    no_worker = subprocess.run([str(executable), str(N)], text=True, capture_output=True, timeout=5)
    assert no_worker.returncode == 2 and "--worker" in no_worker.stderr
    protocol(executable)
    check_coordinator_bind()
    check_registration(executable)
    check_registry_lifecycle()
    check_discovery(executable)
    check_reconnect(executable)
    check_setup_retry(executable)
    check_legacy_replacement(executable)
    check_worker_stall(executable)
    reject_bad_batch(executable)
    bounded_setup(executable, N, 270, force_3lp=True)
    if not args.quick:
        other_worker = args.other_worker.resolve() if args.other_worker else executable
        integration(executable, other_worker, args.timeout)
        recovery(executable, args.timeout, fault_executable)
    if args.large:
        bounded_setup(executable, RSA_110, 364)
        bounded_setup(executable, LARGE_P * LARGE_Q, 499)


if __name__ == "__main__":
    main()
