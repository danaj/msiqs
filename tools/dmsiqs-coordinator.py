#!/usr/bin/env python3
"""In-memory discovery of dmsiqs workers on a trusted network."""

import argparse
from dataclasses import dataclass
import ipaddress
import logging
import math
import re
import socket
import socketserver
import struct
import sys
import threading
import time


MAGIC = b"DMSQ"
VERSION = 1
HEADER = struct.Struct("!4sHHI")
U32 = struct.Struct("!I")
ERROR, REGISTER, REGISTERED, LIST, LISTED = 14, 17, 18, 19, 20
REPORT_STALL, REGISTER_MANAGED, STALLED, REPAIRED = 22, 23, 24, 25
MAX_REQUEST = 2048
MAX_WORKERS = 1024
REQUEST_SECONDS = 3.0
LOG = logging.getLogger("dmsiqs-coordinator")


def read_field(body, offset, length_bytes, limit):
    start = offset + length_bytes
    if start > len(body):
        raise ValueError("truncated field length")
    length = int.from_bytes(body[offset:start], "big")
    end = start + length
    if not 1 <= length <= limit or end > len(body):
        raise ValueError("invalid field length")
    return body[start:end], end


def read_name(body, offset):
    value, offset = read_field(body, offset, 1, 127)
    if any(byte < 32 or byte == 127 for byte in value):
        raise ValueError("control character in name")
    return value, offset


def check_endpoint(value):
    address, _, port = value.rpartition(b":")
    if not re.fullmatch(rb"[0-9]{1,5}", port) or not 1 <= int(port) <= 65535:
        raise ValueError("invalid endpoint port")
    if address.startswith(b"[") and address.endswith(b"]"):
        literal = address[1:-1]
        if not re.fullmatch(rb"[a-zA-Z0-9:.%_-]{1,253}", literal):
            raise ValueError("invalid IPv6 endpoint")
        ipaddress.IPv6Address(literal.decode("ascii"))
    elif not re.fullmatch(rb"[a-zA-Z0-9_.-]{1,253}", address):
        raise ValueError("invalid endpoint host")


def display(value):
    return value.decode("ascii", "backslashreplace")


@dataclass
class Lease:
    endpoint: bytes
    expires: float
    managed: bool
    stalled: bool


class Registry:
    def __init__(self, ttl):
        self.ttl = ttl
        self.pools = {}
        self.pending_stalls = {}
        self.lock = threading.Lock()

    def _expire(self, now):
        for key, expires in list(self.pending_stalls.items()):
            if expires <= now:
                del self.pending_stalls[key]
        for pool, workers in list(self.pools.items()):
            for name, lease in list(workers.items()):
                if lease.expires <= now:
                    del workers[name]
                    LOG.info("%s lease expired in pool %s at %s",
                             display(name), display(pool), display(lease.endpoint))
            if not workers:
                del self.pools[pool]

    def expire(self):
        with self.lock:
            self._expire(time.monotonic())

    def register(self, pool, name, endpoint, managed=False):
        with self.lock:
            now = time.monotonic()
            self._expire(now)
            workers = self.pools.setdefault(pool, {})
            previous = workers.get(name)
            if previous is None and len(workers) == MAX_WORKERS:
                raise ValueError("worker limit reached for pool")
            stalled = False
            if managed:
                stalled = self.pending_stalls.pop((pool, endpoint), None) is not None
                stalled = stalled or any(lease.stalled and lease.endpoint == endpoint
                                         for lease in workers.values())
            workers[name] = Lease(endpoint, now + self.ttl, managed, stalled)
            if previous is None:
                LOG.info("%s %s in pool %s at %s", display(name),
                         "registered as stalled" if stalled else "active", display(pool), display(endpoint))
            elif previous.endpoint != endpoint:
                LOG.info("%s moved in pool %s from %s to %s",
                         display(name), display(pool), display(previous.endpoint), display(endpoint))
            return stalled

    def set_stalled(self, pool, endpoint, stalled):
        with self.lock:
            now = time.monotonic()
            self._expire(now)
            key = (pool, endpoint)
            managed = changed = False
            for lease in self.pools.get(pool, {}).values():
                if lease.managed and lease.endpoint == endpoint:
                    managed = True
                    changed |= lease.stalled != stalled
                    lease.stalled = stalled
            if stalled:
                if not managed:
                    self.pending_stalls[key] = now + self.ttl
                LOG.warning("worker stall reported in pool %s at %s (%s)",
                            display(pool), display(endpoint),
                            "managed endpoint withheld" if managed else "pending managed registration")
            elif self.pending_stalls.pop(key, None) is not None or changed:
                LOG.info("worker repaired in pool %s at %s", display(pool), display(endpoint))

    def endpoints(self, pool):
        with self.lock:
            self._expire(time.monotonic())
            leases = self.pools.get(pool, {}).values()
            stalled = {lease.endpoint for lease in leases if lease.stalled}
            return sorted({lease.endpoint for lease in leases} - stalled)


def set_deadline(connection, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("request timed out")
    connection.settimeout(remaining)


def read_exact(connection, length, deadline):
    data = bytearray(length)
    view = memoryview(data)
    while view:
        set_deadline(connection, deadline)
        filled = connection.recv_into(view)
        if not filled:
            raise EOFError
        view = view[filled:]
    return bytes(data)


class RequestHandler(socketserver.BaseRequestHandler):
    def reply(self, kind, body):
        pool, offset = read_name(body, 0)
        if kind == LIST:
            if offset != len(body):
                raise ValueError("unexpected data after pool name")
            endpoints = self.server.registry.endpoints(pool)
            fields = [U32.pack(len(endpoints))]
            fields.extend(U32.pack(len(endpoint)) + endpoint for endpoint in endpoints)
            return LISTED, b"".join(fields)

        registration = kind in (REGISTER, REGISTER_MANAGED)
        if registration:
            name, offset = read_name(body, offset)
        endpoint, offset = read_field(body, offset, 4, 1024)
        if offset != len(body):
            if registration:
                raise ValueError("unexpected data after registration")
            raise ValueError("unexpected data after endpoint")
        check_endpoint(endpoint)
        if registration:
            if self.server.registry.register(pool, name, endpoint, managed=kind == REGISTER_MANAGED):
                return STALLED, b""
        else:
            self.server.registry.set_stalled(pool, endpoint, stalled=kind == REPORT_STALL)
        return REGISTERED, b""

    def handle(self):
        deadline = time.monotonic() + REQUEST_SECONDS
        try:
            header = read_exact(self.request, HEADER.size, deadline)
            magic, version, kind, length = HEADER.unpack(header)
            if magic != MAGIC or version != VERSION:
                return
            if kind not in (REGISTER, LIST, REPORT_STALL, REGISTER_MANAGED, REPAIRED) or length > MAX_REQUEST:
                raise ValueError("invalid registry request")
            body = read_exact(self.request, length, deadline)
            response, payload = self.reply(kind, body)
        except (EOFError, OSError):
            return
        except ValueError as error:
            response, payload = ERROR, str(error).encode("utf-8")
        try:
            set_deadline(self.request, deadline)
            self.request.sendall(HEADER.pack(MAGIC, VERSION, response, len(payload)) + payload)
        except OSError:
            pass


class Coordinator(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, ttl):
        self.registry = Registry(ttl)
        self.address_family = socket.AF_INET6 if ":" in address[0] else socket.AF_INET
        super().__init__(address, RequestHandler)

    def service_actions(self):
        self.registry.expire()


def listener_address(bind, port):
    inline_port = None
    if bind.startswith("["):
        host, bracket, suffix = bind[1:].partition("]")
        if not bracket or (suffix and not suffix.startswith(":")):
            raise ValueError("invalid --bind address")
        ipaddress.IPv6Address(host)
        if suffix:
            inline_port = suffix[1:]
    elif bind.count(":") == 1:
        host, inline_port = bind.split(":", 1)
    else:
        host = bind
        if ":" in host:
            ipaddress.IPv6Address(host)

    if (not host and inline_port is not None) or "[" in host or "]" in host:
        raise ValueError("invalid --bind address")
    if port is not None and not 0 <= port <= 65535:
        raise ValueError("port must be between 0 and 65535")
    if inline_port is not None:
        if not re.fullmatch(r"[0-9]{1,5}", inline_port) or int(inline_port) > 65535:
            raise ValueError("invalid port in --bind, expected 0 to 65535")
        bind_port = int(inline_port)
        if port is not None and port != bind_port:
            raise ValueError("conflicting ports in --bind and --port")
        port = bind_port
    return host, 8013 if port is None else port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", metavar="ADDRESS[:PORT]", default="127.0.0.1",
                        help="listener address and optional port (default 127.0.0.1:8013)")
    parser.add_argument("--port", type=int, help="listener port for existing scripts")
    parser.add_argument("--ttl", type=float, default=12, help="worker expiry in seconds (default 12)")
    args = parser.parse_args()
    try:
        address = listener_address(args.bind, args.port)
    except ValueError as error:
        parser.error(str(error))
    if not math.isfinite(args.ttl) or args.ttl <= 0:
        parser.error("expiry must be positive and finite")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    try:
        with Coordinator(address, args.ttl) as server:
            host, port = server.server_address[:2]
            endpoint = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
            print(f"dmsiqs coordinator listening on {endpoint}", file=sys.stderr, flush=True)
            server.serve_forever()
    except KeyboardInterrupt:
        return 0
    except OSError as error:
        print(f"dmsiqs coordinator: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
