#!/usr/bin/env python3
"""Launch and maintain a Spot Axion worker pool from the factoring network."""

import argparse
from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from dataclasses import asdict, dataclass
import fcntl
from functools import partial
import getpass
import json
import logging
import os
from pathlib import Path
from queue import Empty, SimpleQueue
import re
import secrets
import socket
import struct
from tempfile import NamedTemporaryFile
import threading
import time
from urllib.parse import urlsplit

from n4a_gce import CreateAttempt, Gce, GceError, ZONES


PROBE_SECONDS = 30
REPAIR_MISSES = 3
RETRY_SECONDS = 120
MAX_WORKERS = 12
WORKER_PORT = 8012
COORDINATOR_PORT = 8013
REGISTRY_HEADER = struct.Struct("!4sHHI")
ERROR, REGISTER, REGISTERED = 14, 17, 18
REGISTER_MANAGED, STALLED, REPAIRED = 23, 24, 25
LOG = logging.getLogger("n4a-manager")


@dataclass(frozen=True)
class Config:
    project: str
    snapshot: str
    vm_user: str
    run_id: str
    count: int
    coordinator: str
    pool: str


def worker_count(value):
    try:
        count = int(value)
    except ValueError:
        count = 0
    if not 1 <= count <= MAX_WORKERS:
        raise argparse.ArgumentTypeError(f"worker count must be between 1 and {MAX_WORKERS}")
    return count


def name_field(value):
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,127}", value):
        raise argparse.ArgumentTypeError("use 1–127 letters, digits, dots, underscores or hyphens")
    return value


def coordinator_socket(address):
    parts = urlsplit("//" + address)
    if not parts.hostname or parts.username is not None or parts.path or parts.query or parts.fragment:
        raise ValueError("coordinator address must be a hostname or IP with an optional port")
    port = parts.port if parts.port is not None else COORDINATOR_PORT
    if port == 0:
        raise ValueError("coordinator port must be greater than zero")
    return parts.hostname, port


def endpoint_for(address):
    return f"[{address}]:{WORKER_PORT}" if ":" in address else f"{address}:{WORKER_PORT}"


def registry_request(address, kind, body):
    with socket.create_connection(coordinator_socket(address), timeout=2) as connection:
        connection.sendall(REGISTRY_HEADER.pack(b"DMSQ", 1, kind, len(body)) + body)
        with connection.makefile("rb") as reply:
            header = reply.read(REGISTRY_HEADER.size)
            if len(header) != REGISTRY_HEADER.size:
                raise OSError("incomplete coordinator reply")
            magic, version, response, length = REGISTRY_HEADER.unpack(header)
            if magic != b"DMSQ" or version != 1 or length > 2048:
                raise OSError("invalid coordinator reply")
            payload = reply.read(length)
            if len(payload) != length:
                raise OSError("incomplete coordinator reply payload")
            return response, payload


def renew(address, pool, name, endpoint):
    pool_bytes, name_bytes, endpoint_bytes = (text.encode("ascii") for text in (pool, name, endpoint))
    body = bytes((len(pool_bytes),)) + pool_bytes + bytes((len(name_bytes),)) + name_bytes
    body += struct.pack("!I", len(endpoint_bytes)) + endpoint_bytes
    response = registry_request(address, REGISTER_MANAGED, body)
    if response == (STALLED, b""):
        return True
    if response == (ERROR, b"invalid registry request"):
        response = registry_request(address, REGISTER, body)
    if response != (REGISTERED, b""):
        raise OSError("coordinator rejected registration")
    return False


def confirm_repaired(config, address):
    pool = config.pool.encode("ascii")
    endpoint = endpoint_for(address).encode("ascii")
    body = bytes((len(pool),)) + pool + struct.pack("!I", len(endpoint)) + endpoint
    if registry_request(config.coordinator, REPAIRED, body) != (REGISTERED, b""):
        raise OSError("coordinator rejected repair confirmation")


def probe_and_renew(instance, config):
    try:
        with socket.create_connection((instance.address, WORKER_PORT), timeout=2):
            pass
    except OSError:
        return False, None
    name = f"axion-{config.run_id}-{instance.slot}"
    try:
        return True, renew(config.coordinator, config.pool, name, endpoint_for(instance.address))
    except OSError as error:
        LOG.warning("%s is listening, but registration failed: %s", instance.name, error)
        return True, None


def inventory(gce, count):
    slots = {}
    for instance in gce.instances():
        if instance.slot not in range(1, count + 1) or instance.slot in slots:
            raise GceError(f"unexpected VM ownership for slot {instance.slot}: {instance.name}")
        slots[instance.slot] = instance
    return slots


def update_create(pending, slot, attempt, save):
    updated = pending.copy()
    if attempt is None:
        del updated[slot]
    else:
        updated[slot] = attempt
    if save is not None:
        save(updated)
    pending.clear()
    pending.update(updated)


def matches_create(instance, attempt):
    return attempt is not None and instance.zone == attempt.zone and instance.generation == attempt.generation


def resolve_creates(gce, slots, pending, save, stopped):
    unavailable = set()
    confirmed = []
    for slot, attempt in list(pending.items()):
        if stopped():
            break
        instance = slots.get(slot)
        listed = instance is not None
        if instance and not matches_create(instance, attempt):
            LOG.warning("Slot %d has a worker other than its reserved create in %s", slot, attempt.zone)
            continue
        if instance is None or instance.status not in ("RUNNING", "TERMINATED"):
            try:
                instance = gce.resolve_create(slot, attempt)
            except GceError as error:
                LOG.warning("%s", error)
                if not error.uncertain:
                    update_create(pending, slot, None, save)
                    if error.capacity:
                        unavailable.add(attempt.zone)
                continue
            if instance is None:
                LOG.warning("Create outcome unknown for %s in %s. Keeping reservation",
                            gce.create_name(slot, attempt), attempt.zone)
                continue
        slots[slot] = instance
        # Keep the reservation until the instance appears in the inventory
        # used for teardown, even if a targeted operation read found it.
        if listed:
            confirmed.append(instance)
    return unavailable, confirmed


def launch_missing(gce, missing, slots, zones, skip_zones, pending_creates, save_creates, publish, stopped):
    if not missing:
        return
    errors = []
    with ThreadPoolExecutor(max_workers=len(missing)) as launches:
        while missing and not stopped():
            occupied = {vm.zone for vm in slots.values()} | skip_zones
            reserved = occupied | {attempt.zone for attempt in pending_creates.values()}
            tasks = {}
            for slot in missing:
                if stopped():
                    break
                zone = next((candidate for candidate in zones if candidate not in reserved), None)
                if zone is None:
                    LOG.warning("No available zone for slot %d", slot)
                    continue
                attempt = CreateAttempt(zone, secrets.token_hex(6))
                # Persist the VM identity before submitting anything to GCE.
                update_create(pending_creates, slot, attempt, save_creates)
                reserved.add(zone)
                zones.rotate(-zones.index(zone) - 1)
                LOG.info("Launching slot %d in %s", slot, zone)
                tasks[launches.submit(gce.create, slot, attempt)] = (slot, zone)

            missing = []
            for task in as_completed(tasks):
                slot, zone = tasks[task]
                try:
                    slots[slot] = task.result()
                except GceError as error:
                    LOG.warning("Launch in %s failed: %s", zone, error)
                    if not error.uncertain:
                        update_create(pending_creates, slot, None, save_creates)
                    if error.capacity and not error.uncertain:
                        skip_zones.add(zone)
                        missing.append(slot)
                    else:
                        errors.append(error)
                else:
                    publish(slots.copy())
            missing.sort()
    if errors:
        raise errors[0]


def restart_service(gce, instance):
    LOG.info("Restarting worker service on %s", instance.name)
    gce.restart_worker(instance)


def repair_service(gce, config, stalls, publish, instance):
    if not stalls.get(instance.slot):
        restart_service(gce, instance)
    if instance.slot in stalls:
        try:
            confirm_repaired(config, instance.address)
        except OSError as error:
            LOG.warning("Worker service restarted on %s, but repair confirmation failed: %s",
                        instance.name, error)
            publish((instance, False))
        else:
            publish((instance, True))


def reconcile(gce, count, unhealthy, publish, stopped, known=None, pending_creates=None,
              zones=None, repair=None, save_creates=None):
    pending_creates = {} if pending_creates is None else pending_creates
    slots = inventory(gce, count) if known is None or unhealthy or pending_creates else known.copy()
    publish(slots.copy())
    skip_zones = set()
    if pending_creates:
        skip_zones, confirmed = resolve_creates(gce, slots, pending_creates, save_creates, stopped)
        for instance in confirmed:
            update_create(pending_creates, instance.slot, None, save_creates)
        publish(slots.copy())
    zones = deque() if zones is None else zones
    repair = partial(restart_service, gce) if repair is None else repair
    if not zones:
        cursor = max((ZONES.index(vm.zone) for vm in slots.values()), default=-1) + 1
        zones.extend(ZONES[cursor:] + ZONES[:cursor])
    for slot in range(1, count + 1):
        if stopped():
            return
        instance = slots.get(slot)
        if slot in pending_creates:
            continue
        if slot in unhealthy and instance and instance.status == "TERMINATED":
            LOG.info("Replacing stopped VM %s", instance.name)
            gce.delete(instance, stopped_only=True)
            del slots[slot]
            skip_zones.add(instance.zone)
            publish(slots.copy())
        elif instance and instance.status == "RUNNING" and instance.address and unhealthy.get(slot) == instance.address:
            try:
                repair(instance)
            except GceError as error:
                LOG.warning("Service restart on %s failed: %s", instance.name, error)
    missing = [slot for slot in range(1, count + 1) if slot not in slots and slot not in pending_creates]
    launch_missing(gce, missing, slots, zones, skip_zones, pending_creates, save_creates, publish, stopped)


def watch(gce, config, known=None, pending_creates=None, save_creates=None):
    slots = {} if known is None else known
    misses, retry_after = {}, {}
    pending_creates = {} if pending_creates is None else pending_creates
    # A stalled slot stays pending until the directory confirms its repair.
    # True means the same VM was restarted and only its acknowledgement is due.
    stalled = {}
    have_inventory = known is not None
    updates = SimpleQueue()
    repair_updates = SimpleQueue()
    stopping = threading.Event()
    zones = deque()
    next_probe = next_incomplete = time.monotonic()
    pending = None
    with ThreadPoolExecutor(max_workers=1) as cloud, ThreadPoolExecutor(max_workers=config.count) as network:
        try:
            while True:
                while True:
                    try:
                        current = updates.get_nowait()
                    except Empty:
                        break
                    have_inventory = True
                    for slot in slots.keys() | current.keys():
                        if slots.get(slot) != current.get(slot):
                            misses.pop(slot, None)
                            retry_after.pop(slot, None)
                            stalled.pop(slot, None)
                            if slot in current:
                                vm = current[slot]
                                LOG.info("Slot %d: %s %s %s", slot, vm.name, vm.status, vm.address or "")
                    slots = current
                while True:
                    try:
                        vm, confirmed = repair_updates.get_nowait()
                    except Empty:
                        break
                    if slots.get(vm.slot) == vm and vm.slot in stalled:
                        if confirmed:
                            stalled.pop(vm.slot)
                        else:
                            stalled[vm.slot] = True
                if pending is not None and pending.done():
                    try:
                        pending.result()
                    except GceError as error:
                        LOG.warning("GCE reconciliation failed: %s", error)
                    pending = None

                now = time.monotonic()
                if now >= next_probe:
                    next_probe = now + PROBE_SECONDS
                    ready = {slot: network.submit(probe_and_renew, vm, config) for slot, vm in slots.items()
                             if vm.address}
                    for slot in slots:
                        listening, reported_stall = ready[slot].result() if slot in ready else (False, None)
                        if reported_stall is True:
                            stalled.setdefault(slot, False)
                        elif reported_stall is False:
                            stalled.pop(slot, None)
                        if listening:
                            misses.pop(slot, None)
                            if slot not in stalled:
                                retry_after.pop(slot, None)
                        else:
                            misses[slot] = misses.get(slot, 0) + 1

                unhealthy = {slot: vm.address for slot, vm in slots.items()
                             if (slot in stalled or misses.get(slot, 0) >= REPAIR_MISSES)
                             and now >= retry_after.get(slot, 0)}
                incomplete = len(slots) < config.count or pending_creates
                if pending is None and (unhealthy or (incomplete and now >= next_incomplete)):
                    repairs = {slot: stalled[slot] and misses.get(slot, 0) < REPAIR_MISSES
                               for slot in unhealthy if slot in stalled}
                    repair = partial(repair_service, gce, config, repairs, repair_updates.put)
                    pending = cloud.submit(reconcile, gce, config.count, unhealthy,
                                           updates.put, stopping.is_set,
                                           slots if have_inventory else None, pending_creates, zones, repair,
                                           save_creates)
                    next_incomplete = now + RETRY_SECONDS
                    retry_after.update({slot: now + RETRY_SECONDS for slot in unhealthy})
                time.sleep(0.25)
        except KeyboardInterrupt:
            LOG.info("Stopping the watcher. Resume with watch or clean up with down.")
        finally:
            stopping.set()


@contextmanager
def state_lock(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_suffix(path.suffix + ".lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("this fleet is in use. Stop its watcher with Ctrl-C first") from error
        yield


def write_state(path, config, pending_creates):
    data = asdict(config)
    data["pending_creates"] = {str(slot): asdict(attempt) for slot, attempt in pending_creates.items()}
    with NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as state:
        temp = Path(state.name)
        try:
            json.dump(data, state, indent=2)
            state.write("\n")
            state.flush()
            os.fsync(state.fileno())
            state.close()
            temp.replace(path)
            fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            temp.unlink(missing_ok=True)


def read_state(path):
    try:
        data = json.loads(path.read_text())
        if not isinstance(data, dict):
            raise ValueError("expected a JSON object")
        pending_data = data.pop("pending_creates", {})
        config = Config(**data)
        worker_count(str(config.count))
        name_field(config.pool)
        coordinator_socket(config.coordinator)
        pending = {}
        if not isinstance(pending_data, dict):
            raise ValueError("invalid create reservations")
        for key, value in pending_data.items():
            slot = int(key)
            if str(slot) != key or slot not in range(1, config.count + 1):
                raise ValueError("invalid slot in create reservations")
            pending[slot] = CreateAttempt(**value)
        return config, pending
    except (TypeError, ValueError, argparse.ArgumentTypeError) as error:
        raise ValueError(f"invalid fleet state in {path}: {error}") from error


def down(gce, state, pending_creates=None, save_creates=None):
    pending_creates = {} if pending_creates is None else pending_creates
    instances = {vm.name: vm for vm in gce.instances()}
    pending_slots = {vm.slot: vm for vm in instances.values()
                     if matches_create(vm, pending_creates.get(vm.slot))}
    _, confirmed = resolve_creates(gce, pending_slots, pending_creates, save_creates, lambda: False)
    confirmed = {vm.name for vm in confirmed}
    instances.update({vm.name: vm for vm in pending_slots.values()})
    failed = False
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as deletions:
        tasks = {}
        for instance in instances.values():
            if matches_create(instance, pending_creates.get(instance.slot)) and instance.name not in confirmed:
                continue
            LOG.info("Deleting %s in %s", instance.name, instance.zone)
            tasks[deletions.submit(gce.delete, instance)] = instance
        for task in as_completed(tasks):
            instance = tasks[task]
            try:
                task.result()
            except GceError as error:
                LOG.warning("Deletion of %s failed: %s", instance.name, error)
                failed = True
            else:
                if instance.name in confirmed:
                    update_create(pending_creates, instance.slot, None, save_creates)
    if pending_creates and not failed:
        raise GceError("fleet still has unresolved creates. State retained. Check GCE operations and retry down")
    remaining = gce.instances()
    if failed:
        raise GceError("fleet has unconfirmed deletes. State retained. Check GCE operations and retry down")
    if remaining:
        raise GceError("fleet still has instances. Run down again after deletion finishes")
    state.unlink()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    parser = argparse.ArgumentParser(description=__doc__)
    default_state = Path.home() / ".dmsiqs" / "n4a.json"
    parser.add_argument("--state", type=Path, default=default_state, help=f"fleet state (default {default_state})")
    commands = parser.add_subparsers(dest="command", required=True)
    up = commands.add_parser("up", help="create a fleet and watch it in the foreground")
    up.add_argument("--project", required=True, help="GCE project ID")
    up.add_argument("--snapshot", default="axion-snap")
    up.add_argument("--vm-user", default=getpass.getuser())
    up.add_argument("-n", "--count", type=worker_count, default=6, help="worker count from 1 to 12 (default 6)")
    up.add_argument("--coord", default="127.0.0.1:8013", help="coordinator configured with --ttl 120")
    up.add_argument("--pool", type=name_field, default="axion")
    commands.add_parser("watch", help="resume an existing fleet")
    commands.add_parser("down", help="delete the fleet after stopping its watcher")
    args = parser.parse_args()
    state = args.state.expanduser().resolve()
    try:
        with state_lock(state):
            if args.command == "up":
                if state.exists():
                    raise ValueError(f"fleet state already exists at {state}. Use watch to resume it")
                coordinator_socket(args.coord)
                config = Config(args.project, args.snapshot, args.vm_user, secrets.token_hex(6),
                                args.count, args.coord, args.pool)
                gce = Gce(config.project, config.snapshot, config.vm_user, config.run_id, WORKER_PORT)
                pending_creates = {}
                write_state(state, config, pending_creates)
                LOG.info("Fleet %s: %d workers in %s", config.run_id, config.count, config.project)
            else:
                config, pending_creates = read_state(state)
                gce = Gce(config.project, config.snapshot, config.vm_user, config.run_id, WORKER_PORT)
            save_creates = partial(write_state, state, config)
            if args.command == "down":
                down(gce, state, pending_creates, save_creates)
            elif args.command == "up":
                watch(gce, config, known={}, pending_creates=pending_creates, save_creates=save_creates)
            else:
                watch(gce, config, pending_creates=pending_creates, save_creates=save_creates)
    except (OSError, ValueError, GceError) as error:
        LOG.error("%s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
