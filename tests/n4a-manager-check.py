#!/usr/bin/env python3
"""Exercise Axion recovery and registry registration without GCE or SSH."""

import argparse
from concurrent.futures import Future
import importlib
import io
import json
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory
from threading import Barrier
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
coordinator = importlib.import_module("dmsiqs-coordinator")
manager = importlib.import_module("n4a-manager")
from n4a_gce import CreateAttempt, Gce, GceError, Instance, ZONES


class FakeGce:
    def __init__(self, workers=()):
        self.workers = {worker.slot: worker for worker in workers}
        self.actions = []
        self.inventory_error = None

    def instances(self):
        if self.inventory_error:
            raise self.inventory_error
        return list(self.workers.values())

    def create(self, slot, attempt):
        assert slot not in self.workers
        self.actions.append(("create", slot, attempt.zone))
        worker = Instance(self.create_name(slot, attempt), attempt.zone, "RUNNING",
                          f"192.0.2.{slot}", slot, attempt.generation)
        self.workers[slot] = worker
        return worker

    def create_name(self, slot, attempt):
        return f"new-{slot}-{attempt.generation}"

    def resolve_create(self, slot, attempt):
        return None

    def restart_worker(self, worker):
        self.actions.append(("restart", worker.name))

    def delete(self, worker, *, stopped_only=False):
        if stopped_only:
            assert self.workers[worker.slot].status == "TERMINATED"
        self.actions.append(("delete", worker.name, stopped_only))
        del self.workers[worker.slot]


class ImmediateExecutor:
    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def submit(self, function, *args):
        future = Future()
        try:
            future.set_result(function(*args))
        except Exception as error:
            future.set_exception(error)
        return future


class FleetCheck(unittest.TestCase):
    def reconcile(self, gce, count=1, unhealthy=None):
        snapshots = []
        with patch.object(manager, "LOG"):
            manager.reconcile(gce, count, unhealthy or {}, snapshots.append, lambda: False)
        return snapshots

    def test_worker_count_bounds(self):
        for value in (1, 6, 12):
            self.assertEqual(manager.worker_count(str(value)), value)
        for value in ("0", "-1", "13", "six"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                manager.worker_count(value)

    def test_missing_slots_launch_concurrently_in_lowest_available_zones(self):
        arrivals = Barrier(2, timeout=3)

        class ParallelCreate(FakeGce):
            def create(self, slot, attempt):
                arrivals.wait()
                return super().create(slot, attempt)

        original = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        gce = ParallelCreate([original])
        snapshots = self.reconcile(gce, count=3)
        self.assertCountEqual(gce.actions, [("create", 2, ZONES[1]), ("create", 3, ZONES[2])])
        self.assertEqual(snapshots[0], {1: original})
        self.assertEqual(snapshots[-1], gce.workers)
        self.assertIs(snapshots[-1][1], original)

    def test_capacity_failure_tries_next_zone(self):
        class FirstZoneUnavailable(FakeGce):
            def create(self, slot, attempt):
                if attempt.zone == ZONES[0]:
                    self.actions.append(("create", slot, attempt.zone))
                    raise GceError("zone capacity exhausted", capacity=True)
                return super().create(slot, attempt)

        gce = FirstZoneUnavailable()
        snapshots = self.reconcile(gce)
        self.assertEqual(gce.actions, [("create", 1, ZONES[0]), ("create", 1, ZONES[1])])
        self.assertEqual(snapshots[-1][1].zone, ZONES[1])

    def test_unhealthy_running_instance_restarts_service(self):
        original = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        gce = FakeGce([original])
        self.reconcile(gce, unhealthy={1: original.address})
        self.assertEqual(gce.actions, [("restart", original.name)])

    def test_new_address_does_not_trigger_restart_for_old_address(self):
        original = Instance("existing", ZONES[0], "RUNNING", "192.0.2.2", 1)
        gce = FakeGce([original])
        snapshots = self.reconcile(gce, unhealthy={1: "192.0.2.1"})
        self.assertEqual(gce.actions, [])
        self.assertEqual(snapshots[-1][1].address, "192.0.2.2")

    def test_startup_inventory_keeps_stopped_instance_until_missed_probes(self):
        original = Instance("existing", ZONES[0], "TERMINATED", None, 1)
        gce = FakeGce([original])
        snapshots = self.reconcile(gce)
        self.assertEqual(gce.actions, [])
        self.assertEqual(snapshots[-1][1], original)

    def test_stopped_instance_is_replaced_after_last_allocated_zone(self):
        original = Instance("existing", ZONES[0], "TERMINATED", None, 1)
        later = Instance("later-allocation", ZONES[3], "RUNNING", "192.0.2.2", 2)
        gce = FakeGce([original, later])
        snapshots = self.reconcile(gce, count=2, unhealthy={1: "192.0.2.1"})
        self.assertEqual(gce.actions, [("delete", original.name, True), ("create", 1, ZONES[4])])
        self.assertEqual(snapshots[-1], gce.workers)
        self.assertIs(snapshots[-1][2], later)

    def test_replacements_continue_in_zone_order_after_wrap(self):
        first = Instance("first", ZONES[-1], "TERMINATED", None, 1)
        second = Instance("second", ZONES[-2], "RUNNING", "192.0.2.2", 2)
        gce = FakeGce([first, second])
        zones = manager.deque()
        manager.reconcile(gce, 2, {1: None}, lambda _: None, lambda: False, zones=zones)
        self.assertEqual(gce.workers[1].zone, ZONES[0])
        gce.workers[2] = Instance(second.name, second.zone, "TERMINATED", None, 2)
        manager.reconcile(gce, 2, {2: second.address}, lambda _: None, lambda: False, zones=zones)
        self.assertEqual(gce.workers[2].zone, ZONES[1])

    def test_inventory_error_leaves_fleet_alone(self):
        gce = FakeGce()
        gce.inventory_error = GceError("inventory unavailable")
        try:
            self.reconcile(gce)
        except GceError:
            pass
        self.assertEqual(gce.actions, [])

    def test_stop_request_prevents_provisioning(self):
        gce = FakeGce()
        manager.reconcile(gce, 6, {}, lambda workers: None, lambda: True)
        self.assertEqual(gce.actions, [])


class LifecycleCheck(unittest.TestCase):
    def test_uncertain_create_survives_restart_and_late_inventory(self):
        config = manager.Config("test-project", "snap", "worker", "testrun", 1, "127.0.0.1", "axion")
        with TemporaryDirectory() as folder:
            state = Path(folder) / "fleet.json"

            def save(pending):
                manager.write_state(state, config, pending)

            class CreateTimedOut(FakeGce):
                def create(self, slot, attempt):
                    self.actions.append(("create", slot, attempt.zone))
                    _, saved = manager.read_state(state)
                    if saved != {slot: attempt}:
                        raise AssertionError("GCE was called before its reservation was persisted")
                    raise GceError("create timed out", uncertain=True)

            gce = CreateTimedOut()
            pending = {}
            with self.assertRaisesRegex(GceError, "create timed out"):
                manager.reconcile(gce, 1, {}, lambda workers: None, lambda: False,
                                  known={}, pending_creates=pending, save_creates=save)
            _, restarted = manager.read_state(state)
            self.assertEqual(restarted, pending)
            attempt = restarted[1]
            manager.reconcile(gce, 1, {}, lambda workers: None, lambda: False,
                              pending_creates=restarted, save_creates=save)
            self.assertEqual(gce.actions, [("create", 1, ZONES[0])])
            self.assertEqual(manager.read_state(state)[1], restarted)
            gce.workers[1] = Instance(gce.create_name(1, attempt), attempt.zone, "RUNNING",
                                      "192.0.2.1", 1, attempt.generation)
            manager.reconcile(gce, 1, {}, lambda workers: None, lambda: False,
                              pending_creates=restarted, save_creates=save)
            self.assertEqual(gce.actions, [("create", 1, ZONES[0])])
            self.assertEqual(manager.read_state(state)[1], {})

    def test_down_retains_state_until_late_create_is_listed(self):
        config = manager.Config("test-project", "snap", "worker", "testrun", 1, "127.0.0.1", "axion")
        attempt = CreateAttempt(ZONES[0], "000000000123")
        gce = FakeGce()
        vm = Instance(gce.create_name(1, attempt), attempt.zone, "RUNNING",
                      "192.0.2.1", 1, attempt.generation)
        with TemporaryDirectory() as folder:
            state = Path(folder) / "fleet.json"

            def save(pending):
                manager.write_state(state, config, pending)

            save({1: attempt})
            for outcome in (None, vm):
                _, restarted = manager.read_state(state)
                with patch.object(gce, "resolve_create", return_value=outcome):
                    with self.assertRaisesRegex(GceError, "State retained"):
                        manager.down(gce, state, restarted, save)
                self.assertEqual(gce.actions, [])
                self.assertEqual(manager.read_state(state)[1], {1: attempt})
            gce.workers[1] = vm
            _, restarted = manager.read_state(state)
            manager.down(gce, state, restarted, save)
            self.assertEqual(gce.actions, [("delete", vm.name, False)])
            self.assertFalse(state.exists())

    def test_down_keeps_state_after_failed_delete_with_empty_inventory(self):
        config = manager.Config("test-project", "snap", "worker", "testrun", 1, "127.0.0.1", "axion")

        class DeleteTimedOut(FakeGce):
            def delete(self, worker, *, stopped_only=False):
                super().delete(worker, stopped_only=stopped_only)
                raise GceError("delete timed out", uncertain=True)

        for attempt in (None, CreateAttempt(ZONES[0], "000000000123")):
            with self.subTest(attempt=attempt), TemporaryDirectory() as folder:
                state = Path(folder) / "fleet.json"
                saved = {1: attempt} if attempt else {}

                def save(pending):
                    manager.write_state(state, config, pending)

                gce = DeleteTimedOut()
                generation = attempt.generation if attempt else None
                name = gce.create_name(1, attempt) if attempt else "existing"
                gce.workers[1] = Instance(name, ZONES[0], "RUNNING", "192.0.2.1", 1, generation)
                save(saved)
                _, pending = manager.read_state(state)
                with self.assertRaisesRegex(GceError, "unconfirmed deletes"):
                    manager.down(gce, state, pending, save)
                self.assertEqual(gce.instances(), [])
                self.assertEqual(manager.read_state(state)[1], saved)

                if attempt:
                    _, pending = manager.read_state(state)
                    with self.assertRaisesRegex(GceError, "unresolved creates"):
                        manager.down(gce, state, pending, save)
                    with patch.object(gce, "resolve_create", side_effect=GceError("already deleted")):
                        manager.down(gce, state, pending, save)
                    self.assertFalse(state.exists())

    def test_down_checks_exact_create_and_deletes_other_owned_workers_in_slot(self):
        config = manager.Config("test-project", "snap", "worker", "testrun", 1, "127.0.0.1", "axion")
        attempt = CreateAttempt(ZONES[1], "000000000123")
        legacy = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        gce = FakeGce([legacy])
        with TemporaryDirectory() as folder:
            state = Path(folder) / "fleet.json"
            manager.write_state(state, config, {1: attempt})
            with patch.object(gce, "resolve_create", return_value=None) as resolve:
                with self.assertRaisesRegex(GceError, "unresolved creates"):
                    manager.down(gce, state, {1: attempt})
                resolve.assert_called_once_with(1, attempt)
            self.assertEqual(gce.actions, [("delete", legacy.name, False)])
            self.assertEqual(manager.read_state(state)[1], {1: attempt})

    def test_up_defaults_to_six_watch_resumes_and_down_removes_state(self):
        original = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        gce = FakeGce([original])
        with TemporaryDirectory() as folder:
            state = Path(folder) / "fleet.json"
            args = ["n4a-manager", "--state", str(state)]
            with patch.object(manager, "Gce", return_value=gce), patch.object(manager, "watch") as watch:
                with patch.object(sys, "argv", args + ["up", "--project", "fake-project"]):
                    self.assertEqual(manager.main(), 0)
                config, pending = manager.read_state(state)
                self.assertEqual(config.count, 6)
                self.assertEqual(pending, {})
                self.assertEqual(watch.call_args.args, (gce, config))
                self.assertEqual(watch.call_args.kwargs["known"], {})
                self.assertEqual(watch.call_args.kwargs["pending_creates"], {})
                # Legacy state without the create reservation field still loads.
                legacy = json.loads(state.read_text())
                del legacy["pending_creates"]
                state.write_text(json.dumps(legacy))
                with patch.object(sys, "argv", args + ["watch"]):
                    self.assertEqual(manager.main(), 0)
                self.assertEqual(watch.call_count, 2)
                self.assertEqual(watch.call_args.args, (gce, config))
                self.assertEqual(watch.call_args.kwargs["pending_creates"], {})
                with patch.object(sys, "argv", args + ["down"]):
                    self.assertEqual(manager.main(), 0)
            self.assertEqual(gce.actions, [("delete", original.name, False)])
            self.assertFalse(state.exists())

    def test_down_attempts_all_deletes_concurrently_and_checks_remaining_vms(self):
        workers = [Instance(f"vm-{slot}", zone, "RUNNING", f"192.0.2.{slot}", slot)
                   for slot, zone in enumerate(ZONES[:3], 1)]

        for failing_slot in (None, 2):
            with self.subTest(failing_slot=failing_slot), TemporaryDirectory() as folder:
                arrivals = Barrier(len(workers), timeout=3)
                inventories = []

                class ParallelDelete(FakeGce):
                    def instances(self):
                        current = super().instances()
                        inventories.append({worker.slot for worker in current})
                        return current

                    def delete(self, worker, *, stopped_only=False):
                        arrivals.wait()
                        if worker.slot == failing_slot:
                            self.actions.append(("delete", worker.name, stopped_only))
                            raise GceError("delete failed")
                        super().delete(worker, stopped_only=stopped_only)

                gce = ParallelDelete(workers)
                state = Path(folder) / "fleet.json"
                state.touch()
                with patch.object(manager, "LOG"):
                    if failing_slot is None:
                        manager.down(gce, state)
                    else:
                        with self.assertRaises(GceError):
                            manager.down(gce, state)
                self.assertCountEqual(gce.actions, [("delete", worker.name, False) for worker in workers])
                self.assertEqual(state.exists(), failing_slot is not None)
                remaining = set() if failing_slot is None else {failing_slot}
                self.assertEqual(inventories, [{worker.slot for worker in workers}, remaining])

    def test_watch_lists_only_after_three_consecutive_probe_failures(self):
        vm = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        gce = FakeGce([vm])
        config = SimpleNamespace(count=1, coordinator="127.0.0.1:8013", pool="axion", run_id="check")
        misses = []
        inventory_misses = []
        now = 0
        brief_drop = False

        def probe(instance, settings):
            nonlocal brief_drop
            if now < 600:
                if now < 180 or brief_drop:
                    return True, False
                brief_drop = True
            misses.append(instance)
            return False, None

        def advance(seconds):
            nonlocal now
            if len(inventory_misses) == 2 or now >= 800:
                raise KeyboardInterrupt
            now += seconds

        def inventory():
            inventory_misses.append(len(misses))
            return list(gce.workers.values())

        clock = SimpleNamespace(monotonic=lambda: now, sleep=advance)
        with patch.object(gce, "instances", inventory), patch.object(manager, "time", clock), \
                patch.object(manager, "ThreadPoolExecutor", return_value=ImmediateExecutor()), \
                patch.object(manager, "probe_and_renew", probe), patch.object(manager, "LOG"):
            manager.watch(gce, config)
        self.assertEqual(inventory_misses, [0, 4])
        self.assertEqual(gce.actions, [("restart", vm.name)])

    def test_watch_repairs_first_stall_and_retries_failed_steps(self):
        for failure, restart_times, clear_times in (
                (None, [0], [0]),
                ("restart", [0, 120], [120]),
                ("clear", [0], [0, 120])):
            with self.subTest(failure=failure):
                vm = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
                gce = FakeGce([vm])
                config = SimpleNamespace(count=1, coordinator="127.0.0.1:8013", pool="axion", run_id="check")
                now = 0
                restarts, clears, inventories = [], [], []
                confirmed = False

                def advance(seconds):
                    nonlocal now
                    if confirmed or now >= 121:
                        raise KeyboardInterrupt
                    now += seconds

                def probe(instance, settings):
                    # The port stays open. A directory outage must not erase a known stall.
                    return True, True if now == 0 else None

                def inventory():
                    inventories.append(now)
                    return [vm]

                def restart(instance):
                    self.assertEqual(instance, vm)
                    restarts.append(now)
                    if failure == "restart" and len(restarts) == 1:
                        raise GceError("restart failed")

                def clear(settings, address):
                    nonlocal confirmed
                    self.assertEqual((settings, address), (config, vm.address))
                    self.assertTrue(restarts)
                    clears.append(now)
                    if failure == "clear" and len(clears) == 1:
                        raise OSError("coordinator unavailable")
                    confirmed = True

                clock = SimpleNamespace(monotonic=lambda: now, sleep=advance)
                with patch.object(gce, "instances", inventory), patch.object(gce, "restart_worker", restart), \
                        patch.object(manager, "time", clock), patch.object(manager, "LOG"), \
                        patch.object(manager, "ThreadPoolExecutor", return_value=ImmediateExecutor()), \
                        patch.object(manager, "probe_and_renew", probe), \
                        patch.object(manager, "confirm_repaired", clear):
                    manager.watch(gce, config, known={1: vm})
                self.assertTrue(confirmed)
                self.assertEqual(restarts, restart_times)
                self.assertEqual(clears, clear_times)
                self.assertEqual(inventories, sorted(set(restart_times + clear_times)))

    def test_watch_discards_pending_confirmation_when_address_changes(self):
        old = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        current = Instance(old.name, old.zone, old.status, "192.0.2.2", old.slot)
        gce = FakeGce([old])
        config = SimpleNamespace(count=1, coordinator="127.0.0.1:8013", pool="axion", run_id="check")
        now = 0
        inventories, clears = [], []

        def advance(seconds):
            nonlocal now
            if now >= 151:
                raise KeyboardInterrupt
            now += seconds

        def inventory():
            inventories.append(now)
            return [old if now == 0 else current]

        def clear(settings, address):
            clears.append(address)
            raise OSError("coordinator unavailable")

        def probe(instance, settings):
            return True, instance.address == old.address

        clock = SimpleNamespace(monotonic=lambda: now, sleep=advance)
        with patch.object(gce, "instances", inventory), patch.object(manager, "time", clock), \
                patch.object(manager, "ThreadPoolExecutor", return_value=ImmediateExecutor()), \
                patch.object(manager, "LOG"), patch.object(manager, "probe_and_renew", probe), \
                patch.object(manager, "confirm_repaired", clear):
            manager.watch(gce, config, known={1: old})
        self.assertEqual(inventories, [0, 120])
        self.assertEqual(gce.actions, [("restart", old.name)])
        self.assertEqual(clears, [old.address])


class GceCreateCheck(unittest.TestCase):
    def setUp(self):
        self.gce = Gce("test-project", "snap", "worker", "r" * 24)
        self.attempt = CreateAttempt(ZONES[-1], "000000000123")
        self.name = self.gce.create_name(12, self.attempt)
        self.target = f"https://example.com/projects/test-project/zones/{self.attempt.zone}/instances/{self.name}"
        self.insert = dict(name="operation-create", operationType="insert", targetLink=self.target,
                           targetId="123456789", status="PENDING")
        self.vm = Instance(self.name, self.attempt.zone, "RUNNING", "192.0.2.1", 12, self.attempt.generation)

    def test_gcloud_disables_partial_inventories_and_create_batching(self):
        result = SimpleNamespace(returncode=0, stdout="[]")
        with patch("n4a_gce.subprocess.run", return_value=result) as run:
            self.gce.instances()
            self.gce.resolve_create(12, self.attempt)
            self.gce._gcloud("instances", "create", self.name)
        for call in run.call_args_list:
            self.assertEqual(call.kwargs["env"]["CLOUDSDK_COMPUTE_ALLOW_PARTIAL_ERROR"], "false")
        self.assertEqual(run.call_args.kwargs["env"]["CLOUDSDK_COMPUTE_FORCE_BATCH_REQUEST"], "false")

    def test_create_polls_the_single_zonal_operation_and_supports_legacy_names(self):
        self.assertLessEqual(len(self.name), 63)
        with patch("n4a_gce.time.sleep"), patch.object(self.gce, "_gcloud", side_effect=(
                json.dumps([self.insert]), json.dumps(dict(self.insert, status="DONE")))) as command, \
                patch.object(self.gce, "_describe", return_value=self.vm) as describe:
            self.assertEqual(self.gce.create(12, self.attempt), self.vm)
        self.assertIn("--async", command.call_args_list[0].args)
        self.assertEqual(command.call_args_list[1].args[:3], ("operations", "describe", "operation-create"))
        self.assertIn(f"--zone={self.attempt.zone}", command.call_args_list[1].args)
        describe.assert_called_once_with(self.name, self.attempt.zone)

        zone = self.attempt.zone
        old_vm = self.gce._instance(dict(name=f"axion-siqs-{'r' * 24}-12-{zone}", zone=zone, status="RUNNING",
                                         labels={"dmsiqs-run": "r" * 24, "dmsiqs-slot": "12"}))
        self.assertIsNone(old_vm.generation)
        self.assertEqual(old_vm.slot, 12)

    def test_resolution_requires_exact_terminal_evidence(self):
        done = dict(self.insert, status="DONE")
        failed = dict(done, error={"errors": [{"code": "ZONE_RESOURCE_POOL_EXHAUSTED"}]})
        wrong = dict(done, targetLink=self.target + "-other")
        delete = dict(done, operationType="delete", name="operation-delete")
        with patch.object(self.gce, "_gcloud") as command, patch.object(self.gce, "_describe") as describe:
            for entries in ([], [wrong], [self.insert], [done, self.insert]):
                command.return_value = json.dumps(entries)
                self.assertIsNone(self.gce.resolve_create(12, self.attempt))
            describe.assert_not_called()

            command.return_value = json.dumps([wrong, failed])
            with self.assertRaises(GceError) as resolved:
                self.gce.resolve_create(12, self.attempt)
            self.assertFalse(resolved.exception.uncertain)
            self.assertTrue(resolved.exception.capacity)

            command.return_value = json.dumps([done, dict(delete, targetId="987654321")])
            describe.side_effect = GceError("unlisted")
            with self.assertRaises(GceError) as unresolved:
                self.gce.resolve_create(12, self.attempt)
            self.assertTrue(unresolved.exception.uncertain)

            command.return_value = json.dumps([done, delete])
            with self.assertRaisesRegex(GceError, "already been deleted") as removed:
                self.gce.resolve_create(12, self.attempt)
            self.assertFalse(removed.exception.uncertain)


class CoordinatorCheck(unittest.TestCase):
    class FakeConnection:
        def __init__(self, reply=b""):
            self.reply = reply
            self.request = bytearray()

        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

        def sendall(self, data):
            self.request.extend(data)

        def makefile(self, mode):
            return io.BytesIO(self.reply)

    @staticmethod
    def frame(kind, body=b""):
        return struct.pack("!4sHHI", b"DMSQ", 1, kind, len(body)) + body

    def test_renew_uses_managed_registry_protocol(self):
        connection = self.FakeConnection(self.frame(18))
        with patch.object(manager.socket, "create_connection", return_value=connection) as connect:
            stalled = manager.renew("127.0.0.1:8013", "axion", "slot-1", "192.0.2.1:8012")
        self.assertFalse(stalled)
        connect.assert_called_once()
        self.assertEqual(connect.call_args.args[0], ("127.0.0.1", 8013))
        endpoint = b"192.0.2.1:8012"
        body = b"\x05axion\x06slot-1" + struct.pack("!I", len(endpoint)) + endpoint
        self.assertEqual(connection.request, self.frame(23, body))

    def test_listening_worker_can_report_stall(self):
        worker = self.FakeConnection()
        registry = self.FakeConnection(self.frame(24))
        vm = Instance("existing", ZONES[0], "RUNNING", "192.0.2.1", 1)
        config = SimpleNamespace(coordinator="127.0.0.1:8013", pool="axion", run_id="check")
        with patch.object(manager.socket, "create_connection", side_effect=[worker, registry]) as connect:
            self.assertEqual(manager.probe_and_renew(vm, config), (True, True))
        self.assertEqual(connect.call_args_list[0].args[0], ("192.0.2.1", 8012))
        self.assertEqual(connect.call_args_list[1].args[0], ("127.0.0.1", 8013))

    def test_renew_falls_back_on_legacy_error(self):
        old = self.FakeConnection(self.frame(14, b"invalid registry request"))
        fallback = self.FakeConnection(self.frame(18))
        with patch.object(manager.socket, "create_connection", side_effect=[old, fallback]):
            self.assertFalse(manager.renew("127.0.0.1", "axion", "slot-1", "192.0.2.1:8012"))
        body = old.request[manager.REGISTRY_HEADER.size:]
        self.assertEqual(old.request, self.frame(23, body))
        self.assertEqual(fallback.request, self.frame(17, body))
        for reply in (self.frame(14, b"invalid registry request")[:-1], self.frame(14, b"invalid pool")):
            with self.subTest(reply=reply):
                connection = self.FakeConnection(reply)
                with patch.object(manager.socket, "create_connection", return_value=connection) as connect:
                    with self.assertRaises(OSError):
                        manager.renew("127.0.0.1", "axion", "slot-1", "192.0.2.1:8012")
                connect.assert_called_once()

    def test_confirm_repaired_uses_pool_and_endpoint(self):
        config = SimpleNamespace(coordinator="127.0.0.1", pool="axion")
        connection = self.FakeConnection(self.frame(18))
        with patch.object(manager.socket, "create_connection", return_value=connection):
            manager.confirm_repaired(config, "192.0.2.1")
        endpoint = b"192.0.2.1:8012"
        body = b"\x05axion" + struct.pack("!I", len(endpoint)) + endpoint
        self.assertEqual(connection.request, self.frame(25, body))


class CoordinatorStallCheck(unittest.TestCase):
    def setUp(self):
        handler = coordinator.RequestHandler.__new__(coordinator.RequestHandler)
        handler.server = SimpleNamespace(registry=coordinator.Registry(120))
        self.reply = handler.reply
        self.pool = b"\x05axion"
        self.name = b"\x06slot-1"
        endpoint = b"192.0.2.1:8012"
        self.endpoint = coordinator.U32.pack(len(endpoint)) + endpoint

    def test_stall_report_survives_first_managed_registration(self):
        with patch.object(coordinator, "LOG"):
            self.assertEqual(self.reply(coordinator.REPORT_STALL, self.pool + self.endpoint),
                             (coordinator.REGISTERED, b""))
            self.assertEqual(self.reply(coordinator.REGISTER_MANAGED, b"\x03lab" + self.name + self.endpoint),
                             (coordinator.REGISTERED, b""))
            self.assertEqual(self.reply(coordinator.REGISTER_MANAGED, self.pool + self.name + self.endpoint),
                             (coordinator.STALLED, b""))
            self.assertEqual(self.reply(coordinator.LIST, self.pool),
                             (coordinator.LISTED, coordinator.U32.pack(0)))
            self.assertEqual(self.reply(coordinator.REPAIRED, self.pool + self.endpoint),
                             (coordinator.REGISTERED, b""))
            self.assertEqual(self.reply(coordinator.LIST, self.pool),
                             (coordinator.LISTED, coordinator.U32.pack(1) + self.endpoint))

    def test_unmatched_stall_report_expires(self):
        with patch.object(coordinator, "time", SimpleNamespace(monotonic=lambda: 0)), patch.object(coordinator, "LOG"):
            self.reply(coordinator.REPORT_STALL, self.pool + self.endpoint)
        with patch.object(coordinator, "time", SimpleNamespace(monotonic=lambda: 121)), patch.object(coordinator, "LOG"):
            self.assertEqual(self.reply(coordinator.REGISTER_MANAGED, self.pool + self.name + self.endpoint),
                             (coordinator.REGISTERED, b""))


if __name__ == "__main__":
    unittest.main()
