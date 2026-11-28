"""Google Compute Engine operations for the local Axion fleet manager."""

from dataclasses import dataclass
import ipaddress
import json
import os
import re
import shlex
import subprocess
from tempfile import NamedTemporaryFile
import time


ZONES = (
    "us-central1-a",
    "us-central1-b",
    "us-central1-c",
    "us-east1-b",
    "us-east1-c",
    "us-east1-d",
    "us-east4-a",
    "us-east4-b",
    "us-east4-c",
    "us-east5-a",
    "us-east5-b",
    "us-south1-a",
    "us-south1-b",
    "europe-west1-b",
    "europe-west4-a",
    "asia-southeast1-a",
    "asia-southeast1-b",
    "asia-southeast1-c",
)

RUN_LABEL = "dmsiqs-run"
SLOT_LABEL = "dmsiqs-slot"
CREATE_LABEL = "dmsiqs-create"
SERVICE = "dmsiqs-worker"
OPERATION_TIMEOUT = 180


class GceError(RuntimeError):
    def __init__(self, message, *, uncertain=False, capacity=False):
        super().__init__(message)
        self.uncertain = uncertain
        self.capacity = capacity


@dataclass(frozen=True)
class Instance:
    name: str
    zone: str
    status: str
    address: str | None
    slot: int
    generation: str | None = None


@dataclass(frozen=True)
class CreateAttempt:
    zone: str
    generation: str

    def __post_init__(self):
        if self.zone not in ZONES or not re.fullmatch(r"[0-9a-f]{12}", self.generation):
            raise ValueError("invalid reserved Axion create")


def capacity_error(detail):
    return bool(re.search(
        r"resource.pool.exhausted|pool.capacity.insufficient|"
        r"does not have enough resources|is not available in zone|"
        r"does not exist in zone|quota[^\n]*(?:exceed|exhaust)|"
        r"exceed[^\n]*quota",
        detail, re.IGNORECASE,
    ))


class Gce:
    def __init__(self, project, snapshot, vm_user, run_id, worker_port=8012):
        if not re.fullmatch(r"[a-z0-9]{1,24}", run_id):
            raise ValueError("invalid Axion run ID")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*", vm_user):
            raise ValueError("invalid Axion VM user")
        if not isinstance(worker_port, int) or not 1 <= worker_port <= 65535:
            raise ValueError("invalid Axion worker port")
        self.project = project
        self.snapshot = snapshot
        self.vm_user = vm_user
        self.run_id = run_id
        self.worker_port = worker_port

    def _gcloud(self, *args, timeout=30, mutating=False):
        command = ["gcloud", "compute", *args, f"--project={self.project}", "--quiet"]
        action = shlex.join(command[:5])
        # Recovery and teardown require a complete inventory.
        env = dict(os.environ, CLOUDSDK_COMPUTE_ALLOW_PARTIAL_ERROR="false")
        if args[:2] == ("instances", "create"):
            # Keep a single asynchronous insert on the SDK's non-retrying path.
            env["CLOUDSDK_COMPUTE_FORCE_BATCH_REQUEST"] = "false"
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env)
        except subprocess.TimeoutExpired as exc:
            raise GceError(f"{action} timed out after {timeout} seconds", uncertain=mutating) from exc
        except OSError as exc:
            raise GceError(f"{action}: {exc}") from exc
        if result.returncode:
            detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
            capacity = result.returncode > 0 and capacity_error(detail)
            raise GceError(f"{action}: {detail}", uncertain=mutating and not capacity, capacity=capacity)
        return result.stdout

    @staticmethod
    def _json(output):
        try:
            return json.loads(output)
        except ValueError as exc:
            raise GceError("gcloud returned invalid JSON") from exc

    def _name(self, slot, zone, generation=None):
        return f"axion-siqs-{self.run_id}-{slot}-{generation if generation is not None else zone}"

    def create_name(self, slot, attempt):
        return self._name(slot, attempt.zone, attempt.generation)

    def _instance(self, resource):
        try:
            labels = resource.get("labels", {})
            if labels.get(RUN_LABEL) != self.run_id:
                return None
            slot = int(labels[SLOT_LABEL])
            generation = labels.get(CREATE_LABEL)
            if generation is not None and not re.fullmatch(r"[0-9a-f]{12}", generation):
                raise ValueError("invalid Axion create label")
            name = resource["name"]
            zone = resource["zone"].rsplit("/", 1)[-1]
            status = resource["status"]
            interfaces = resource.get("networkInterfaces", [])
            configs = interfaces[0].get("accessConfigs", []) if interfaces else []
            address = next((cfg["natIP"] for cfg in configs if cfg.get("natIP")), None)
            if address is not None:
                address = str(ipaddress.IPv4Address(address))
        except (ValueError, AttributeError, KeyError, TypeError) as exc:
            raise GceError("gcloud returned an invalid Axion instance") from exc
        if not 1 <= slot <= 12 or zone not in ZONES or name != self._name(slot, zone, generation):
            raise GceError(f"invalid ownership labels or name for Axion instance {name}")
        if not isinstance(status, str) or not status:
            raise GceError(f"missing status for Axion instance {name}")
        return Instance(name, zone, status, address, slot, generation)

    def instances(self):
        resources = self._json(self._gcloud(
            "instances", "list", f"--filter=labels.{RUN_LABEL}={self.run_id}", "--format=json"
        ))
        if not isinstance(resources, list):
            raise GceError("gcloud returned an invalid instance list")
        owned = []
        for resource in resources:
            instance = self._instance(resource)
            if instance is not None:
                owned.append(instance)
        return owned

    def _describe(self, name, zone):
        resource = self._json(self._gcloud(
            "instances", "describe", name, f"--zone={zone}", "--format=json"
        ))
        instance = self._instance(resource)
        if instance is None or instance.name != name or instance.zone != zone:
            raise GceError(f"{name} in {zone} does not belong to Axion run {self.run_id}")
        return instance

    def _after_operation(self, name, zone):
        try:
            return self._describe(name, zone)
        except GceError as exc:
            raise GceError(str(exc), uncertain=True) from exc

    def _startup_script(self):
        directory = shlex.quote(f"/home/{self.vm_user}/src/dmsiqs-worker")
        user = shlex.quote(self.vm_user)
        return f"""#!/usr/bin/env bash
set -euo pipefail
META="http://169.254.169.254/computeMetadata/v1/instance"
metadata() {{
  curl -fsS --connect-timeout 3 --max-time 10 --retry 5 --retry-max-time 60 --retry-connrefused \\
    -H "Metadata-Flavor: Google" "$META/$1"
}}
LOCAL_IP=$(metadata network-interfaces/0/ip)
VM_NAME=$(metadata name)
if [[ -z $LOCAL_IP || -z $VM_NAME ]]; then
  printf 'ERROR: VM metadata did not provide an internal IP and name.\\n' >&2
  exit 1
fi
THREADS=$(nproc)

exec systemd-run --unit={SERVICE} --uid={user} --working-directory={directory} \\
  {directory}/dmsiqs --mode worker --bind "$LOCAL_IP" --port {self.worker_port} --name "$VM_NAME" -t "$THREADS"
"""

    @staticmethod
    def _operation_for(operation, verb, name, zone):
        return (isinstance(operation, dict) and operation.get("operationType") == verb
                and isinstance(operation.get("targetLink"), str)
                and operation["targetLink"].endswith(f"/zones/{zone}/instances/{name}"))

    def _create_status(self, operation, name, zone):
        if not self._operation_for(operation, "insert", name, zone) or operation.get("status") not in (
                "PENDING", "RUNNING", "DONE"):
            raise GceError(f"invalid create operation for {name} in {zone}", uncertain=True)
        if operation["status"] != "DONE":
            return None
        if "error" in operation or "httpErrorStatusCode" in operation:
            errors = operation.get("error")
            entries = errors.get("errors") if isinstance(errors, dict) else None
            if not entries or not isinstance(entries, list) or not all(
                    isinstance(item, dict) and isinstance(item.get("code"), str) for item in entries):
                raise GceError(f"unrecognized create failure for {name} in {zone}", uncertain=True)
            detail = ", ".join(f"{item['code']}: {item.get('message', '')}" for item in entries)
            if any("ALREADY_EXISTS" in item["code"] for item in entries):
                raise GceError(f"create of {name} in {zone}: {detail}", uncertain=True)
            raise GceError(f"create of {name} in {zone}: {detail}", capacity=capacity_error(detail))
        return self._after_operation(name, zone)

    def resolve_create(self, slot, attempt):
        """Resolve an interrupted create only with cloud evidence."""
        name, zone = self.create_name(slot, attempt), attempt.zone
        suffix = f"/zones/{zone}/instances/{name}"
        try:
            operations = self._json(self._gcloud(
                "operations", "list", f"--zones={zone}", "--format=json",
                f"--filter=targetLink~'{suffix}$'",
            ))
            if not isinstance(operations, list):
                raise GceError("gcloud returned an invalid operation list")
        except GceError as error:
            raise GceError(str(error), uncertain=True) from error
        matches = [operation for operation in operations if self._operation_for(operation, "insert", name, zone)]
        if len(matches) != 1:
            return None
        insert = matches[0]
        target_id = str(insert.get("targetId", ""))
        if (insert.get("status") == "DONE" and "error" not in insert
                and "httpErrorStatusCode" not in insert and target_id.isascii() and target_id.isdigit()):
            # A completed delete for the same GCE instance ID proves what
            # happened even if the VM was removed before we ever listed it.
            for operation in operations:
                if (self._operation_for(operation, "delete", name, zone)
                        and str(operation.get("targetId")) == target_id
                        and operation.get("status") == "DONE"
                        and "error" not in operation and "httpErrorStatusCode" not in operation):
                    raise GceError(f"{name} in {zone} was created and has already been deleted")
        return self._create_status(insert, name, zone)

    def create(self, slot, attempt):
        if not 1 <= slot <= 12:
            raise ValueError("invalid Axion worker slot")
        name, zone = self.create_name(slot, attempt), attempt.zone
        labels = f"{RUN_LABEL}={self.run_id},{SLOT_LABEL}={slot},{CREATE_LABEL}={attempt.generation}"
        deadline = time.monotonic() + OPERATION_TIMEOUT
        with NamedTemporaryFile(mode="w", prefix="dmsiqs-axion-", suffix=".sh") as startup:
            startup.write(self._startup_script())
            startup.flush()
            output = self._gcloud(
                "instances", "create", name, f"--zone={zone}",
                "--machine-type=n4a-highcpu-64", "--provisioning-model=SPOT",
                "--instance-termination-action=STOP", f"--source-snapshot={self.snapshot}",
                "--boot-disk-type=hyperdisk-balanced", f"--labels={labels}",
                f"--metadata-from-file=startup-script={startup.name}", "--async", "--format=json",
                timeout=OPERATION_TIMEOUT, mutating=True,
            )
        try:
            operations = self._json(output)
            if not isinstance(operations, list) or len(operations) != 1:
                raise GceError(f"expected one create operation for {name}")
            operation = operations[0]
            operation_name = operation.get("name") if isinstance(operation, dict) else None
            if not isinstance(operation_name, str) or not re.fullmatch(r"[a-z0-9-]{1,127}", operation_name):
                raise GceError(f"invalid create operation name for {name}")
        except GceError as error:
            raise GceError(str(error), uncertain=True) from error
        while True:
            instance = self._create_status(operation, name, zone)
            if instance is not None:
                return instance
            remaining = deadline - time.monotonic()
            if remaining <= 3:
                raise GceError(f"create operation {operation_name} still pending for {name}", uncertain=True)
            time.sleep(3)
            try:
                operation = self._json(self._gcloud(
                    "operations", "describe", operation_name, f"--zone={zone}", "--format=json",
                    timeout=min(30, remaining - 3),
                ))
            except GceError as error:
                raise GceError(str(error), uncertain=True) from error
            if not isinstance(operation, dict) or operation.get("name") != operation_name:
                raise GceError(f"invalid status for create operation {operation_name}", uncertain=True)

    def _ensure_owned(self, instance):
        if self._name(instance.slot, instance.zone, instance.generation) != instance.name:
            raise GceError(f"{instance.name} does not belong to Axion run {self.run_id}")
        return self._describe(instance.name, instance.zone)

    def restart_worker(self, instance):
        current = self._ensure_owned(instance)
        if current.status != "RUNNING":
            raise GceError(f"{current.name} is {current.status}")
        # A stopped transient unit may have been unloaded. The guest agent can
        # run the stored startup script again to re-create it.
        command = f"sudo -n systemctl restart {SERVICE}.service || sudo -n google_metadata_script_runner startup"
        self._gcloud(
            "ssh", f"{self.vm_user}@{current.name}", f"--zone={current.zone}",
            "--ssh-flag=-oBatchMode=yes", "--ssh-flag=-oConnectTimeout=10",
            f"--command={command}", timeout=45,
        )

    def delete(self, instance, *, stopped_only=False):
        current = self._ensure_owned(instance)
        if stopped_only and current.status != "TERMINATED":
            raise GceError(f"{current.name} is now {current.status}. Recheck the worker before replacing it")
        self._gcloud(
            "instances", "delete", current.name, f"--zone={current.zone}", "--format=none",
            timeout=OPERATION_TIMEOUT, mutating=True,
        )
