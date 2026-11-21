#!/usr/bin/env python3
"""Fetch FactorDB work, factor a batch, and report verified partitions.

Adapted from James Heinrich's factordb.php (2026-10-24).
Uses Python's standard library; no PHP, curl, or third-party modules.
"""

import argparse
import ast
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import contextmanager
import http.client
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import ssl
from statistics import median
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit


class Error(Exception):
    pass


class Retryable(Error):
    pass


class Stopped(Exception):
    pass


_progress_width = 0


def finish_progress():
    global _progress_width
    if _progress_width:
        _progress_width = 0
        print(flush=True)


def factoring_progress(n):
    global _progress_width
    token = f' C{len(n)}'
    if _progress_width + len(token) > 78:
        finish_progress()
    if not _progress_width:
        token = 'Factoring' + token
    print(token, end='', flush=True)
    _progress_width += len(token)


def say(message):
    finish_progress()
    print(time.strftime('%Y-%m-%d %H:%M:%S'), message, flush=True)


def duration(seconds):
    return f'{seconds * 1000:.3g} ms' if seconds < 1 else f'{seconds:.3g} s'


def number(value):
    """Handle the API's occasional expression despite terms=false, without eval."""
    if type(value) not in (int, str):
        raise Error('FactorDB returned a non-integer work item')
    text = str(value).strip()
    if re.fullmatch(r'[0-9]{1,1300}', text):
        result = int(text)
    else:
        try:
            if len(text) > 5000:
                raise ValueError('expression too large')
            tree = ast.parse(text.replace('^', '**'), mode='eval')
            if sum(1 for _ in ast.walk(tree)) > 128:
                raise ValueError('expression too large')

            def evaluate(node, depth=0):
                if depth > 32:
                    raise ValueError('expression too deep')
                if isinstance(node, ast.Constant) and type(node.value) is int:
                    answer = node.value
                elif isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
                    answer = evaluate(node.operand, depth + 1)
                    if isinstance(node.op, ast.USub):
                        answer = -answer
                elif isinstance(node, ast.BinOp):
                    a, b = evaluate(node.left, depth + 1), evaluate(node.right, depth + 1)
                    if isinstance(node.op, ast.Add):
                        answer = a + b
                    elif isinstance(node.op, ast.Sub):
                        answer = a - b
                    elif isinstance(node.op, ast.Mult):
                        answer = a * b
                    elif isinstance(node.op, (ast.Div, ast.FloorDiv)) and b and a % b == 0:
                        answer = a // b
                    elif isinstance(node.op, ast.Pow) and 0 <= b <= 4096 and a.bit_length() * b <= 8192:
                        answer = a ** b
                    else:
                        raise ValueError('unsupported arithmetic')
                else:
                    raise ValueError('unsupported expression')
                if answer.bit_length() > 8192:
                    raise ValueError('expression too large')
                return answer

            result = evaluate(tree.body)
        except (SyntaxError, ValueError, ZeroDivisionError, RecursionError) as exc:
            raise Error('FactorDB returned an unsupported integer expression') from exc
    if result < 2:
        raise Error('Work items must be integers greater than one')
    return str(result)


def checked_factors(n, factors):
    if type(n) not in (str, int) or not re.fullmatch(r'[0-9]+', str(n)):
        raise Error('Invalid factorization target')
    if not isinstance(factors, list) or len(factors) < 2:
        raise Error('Factoring backend did not return a proper split')
    if any(type(f) not in (str, int) or not re.fullmatch(r'[0-9]+', str(f)) for f in factors):
        raise Error('Non-integer factor in result')
    values = list(map(int, factors))
    if any(f <= 1 or f >= int(n) for f in values) or math.prod(values) != int(n):
        raise Error('Factor product does not match the input')
    return list(map(str, sorted(values)))


def _kill(process):
    if os.name == 'posix':
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    elif process.poll() is None:
        process.kill()


def _dispose(process):
    if process.stdin is not None:
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        _kill(process)
        process.wait()
    for pipe in (process.stdout, process.stderr):
        if pipe is not None:
            pipe.close()


_YAFU_FACTOR = re.compile(r'(PRP|P|C|U)[0-9]+\s*=\s*([0-9]+)')


def _yafu_factors(stdout):
    _, marker, block = stdout.rpartition('***factors found***')
    if not marker:
        raise Error('Could not find the YAFU factors block')
    factors = []
    for line in block.lstrip().splitlines():
        match = _YAFU_FACTOR.fullmatch(line.strip())
        if match is None:
            break
        kind, factor = match.groups()
        if kind not in ('P', 'PRP'):
            raise Error('YAFU returned an incomplete factorization')
        factors.append(factor)
    return factors


def _copy_yafu_ini(executable, directory):
    source = Path(executable).with_name('yafu.ini')
    if not source.is_file():
        return
    # Rebase tool and parameter paths while keeping relative job files local.
    paths = {'ggnfs_dir', 'cado_dir', 'convert_poly_path', 'ecm_path', 'nfs_params', 'cuda_sieve'}
    destination = Path(directory) / 'yafu.ini'
    options = dict(encoding='utf-8', errors='surrogateescape', newline='')
    with source.open('r', **options) as original, destination.open('w', **options) as copy:
        for line in original:
            setting = line.rstrip('\r\n')
            key, _, value = setting.partition('=')
            if key in paths and value and not os.path.isabs(value):
                line = f'{key}={os.path.join(source.parent, value)}' + line[len(setting):]
            copy.write(line)


class FactorSlot:
    """At most one active call per slot. The controller can cancel it."""

    def __init__(self, cfg):
        self.method, self.executable = cfg['method'], cfg['executable']
        self.threads = cfg['threads']
        self.lock = threading.Lock()
        self.process = None
        self.active = self.closed = False

    def _start(self, command, *, persistent=False, data=None, cwd=None):
        with self.lock:
            if self.closed:
                raise Error('Factoring worker interrupted')
            if self.process is not None and self.process.poll() is not None:
                _dispose(self.process)
                self.process = None
            if self.process is None:
                env = os.environ.copy()
                env.pop('FDB_USER_TOKEN', None)
                self.process = subprocess.Popen(
                    command, cwd=cwd, env=env,
                    stdin=subprocess.PIPE if persistent or data is not None else subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=None if persistent else subprocess.PIPE,
                    text=True, encoding='utf-8', errors='replace',
                    start_new_session=(os.name == 'posix'),
                )
            self.active = True
            return self.process

    def _finish(self, process, keep=False):
        with self.lock:
            self.active = False
            if keep and not self.closed:
                return
            self.process = None
        _dispose(process)

    def close(self, interrupt=False):
        with self.lock:
            self.closed = True
            process = self.process
            if process is None:
                return
            if interrupt:
                _kill(process)
            if self.active:
                # The active caller owns the pipes through its final read.
                return
            self.process = None
        _dispose(process)

    def _native_factor(self, n):
        process = self._start([self.executable, '--mode', 'factor'], persistent=True)
        keep = False
        try:
            process.stdin.write(f'factor\t{n}\n')
            process.stdin.flush()
            line = process.stdout.readline(65537)
            if not line.endswith('\n'):
                raise Error('MPU-GMP worker exited or returned a truncated result')
            fields = line.rstrip('\n').split('\t')
            if len(fields) != 4 or fields[:2] != ['ok', n]:
                raise Error('MPU-GMP worker returned an unexpected result')
            factors = fields[2].split()
            if fields[3] != 'p' * len(factors):
                raise Error('MPU-GMP worker returned an incomplete factorization')
            result = checked_factors(n, factors)
            keep = True
            return result
        except OSError as exc:
            raise Error(f'MPU-GMP worker pipe failed: {exc}') from exc
        finally:
            self._finish(process, keep)

    def _run_once(self, command, *, data=None, cwd=None):
        process = self._start(command, data=data, cwd=cwd)
        try:
            stdout, stderr = process.communicate(data)
            if process.returncode:
                tail = (stderr.strip() or stdout.strip())[-800:]
                raise Error(f'{self.method} exited with code {process.returncode}: {tail}')
            return stdout
        finally:
            self._finish(process)

    def factor(self, n):
        if not re.fullmatch(r'[0-9]+', n):
            raise Error('Expected a decimal integer')
        if self.method == 'mpu':
            return self._native_factor(n)
        if self.method == 'yafu':
            expr = f'factor({n})'
            # Unix YAFU reads expressions from stdin when it is a pipe.
            with tempfile.TemporaryDirectory(prefix='factordb-yafu-') as directory:
                _copy_yafu_ini(self.executable, directory)
                stdout = self._run_once([self.executable, expr, '-threads', str(self.threads)],
                                        data=expr + '\n', cwd=directory)
            return checked_factors(n, _yafu_factors(stdout))
        thread_args = ['-t', str(self.threads)] if self.threads > 1 else []
        stdout = self._run_once([self.executable, *thread_args, n])
        lines = [line for line in stdout.splitlines() if line.startswith(n + ':')]
        if len(lines) != 1:
            raise Error('Could not identify the msiqs result line')
        return checked_factors(n, lines[0].split(':', 1)[1].split())


def timed_factor(slot, n):
    start = time.monotonic()
    factors = slot.factor(n)
    return factors, time.monotonic() - start


class Client:
    """One reusable HTTP connection; all report calls in a batch share a POST."""

    def __init__(self, cfg):
        self.url = urlsplit(cfg['api_url'])
        if self.url.scheme not in ('http', 'https') or not self.url.hostname or self.url.username:
            raise Error('api_url must be an HTTP or HTTPS endpoint')
        self.token, self.timeout = cfg['fdb_user_token'], cfg['http_timeout']
        self.connection = None
        self.sequence = 0

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def call(self, method, params):
        self.sequence += 1
        return dict(jsonrpc='2.0', id=self.sequence, method=method, params=params)

    def exchange(self, payload):
        try:
            if self.connection is None:
                cls = http.client.HTTPSConnection if self.url.scheme == 'https' else http.client.HTTPConnection
                kwargs = dict(timeout=self.timeout)
                if self.url.scheme == 'https':
                    kwargs['context'] = ssl.create_default_context()
                self.connection = cls(self.url.hostname, self.url.port, **kwargs)
            path = self.url.path or '/'
            if self.url.query:
                path += '?' + self.url.query
            self.connection.request('POST', path, json.dumps(payload).encode('utf-8'), {
                'Content-Type': 'application/json', 'Accept': 'application/json',
                'User-Agent': 'msiqs-factordb/1.0', 'X-Fdb-User-Token': self.token,
            })
            response = self.connection.getresponse()
            data = response.read()
            if response.status != 200:
                self.close()
                cls = Retryable if response.status == 429 or response.status >= 500 else Error
                raise cls(f'FactorDB HTTP {response.status} {response.reason}')
        except ssl.SSLCertVerificationError as exc:
            self.close()
            raise Error('FactorDB TLS certificate verification failed') from exc
        except (OSError, http.client.HTTPException) as exc:
            self.close()
            raise Retryable(f'FactorDB connection failed: {exc}') from exc
        try:
            replies = json.loads(data)
        except (ValueError, UnicodeError) as exc:
            self.close()
            raise Retryable('FactorDB returned invalid JSON') from exc
        requests = payload if isinstance(payload, list) else [payload]
        replies = replies if isinstance(replies, list) else [replies]
        if not all(isinstance(r, dict) and r.get('jsonrpc') == '2.0' for r in replies):
            raise Error('Invalid FactorDB JSON-RPC response')
        for reply in replies:
            if reply.get('error') is not None:
                message = json.dumps(reply['error']).replace(self.token, '<token>')[:300]
                raise Error('FactorDB RPC error: ' + message)
            if 'result' not in reply:
                raise Error('FactorDB response has no result')
        if not all(type(r.get('id')) is int for r in replies):
            raise Error('Invalid FactorDB response ID')
        if len(replies) != len(requests) or {r['id'] for r in replies} != {r['id'] for r in requests}:
            raise Error('FactorDB response IDs do not match the request')
        return {r['id']: r['result'] for r in replies}

    def download(self, digits, count):
        call = self.call('download', dict(table='C', digits=digits, count=count, random=True, terms=False))
        result = self.exchange(call)[call['id']]
        if not isinstance(result, dict) or not isinstance(result.get('numbers'), list):
            raise Error('FactorDB download response has no numbers array')
        return result['numbers']

    def submit(self, results):
        calls = [self.call('report_factors', dict(target={'expr': r['n']}, factors=r['factors'], credit=True)) for r in results]
        self.exchange(calls)


@contextmanager
def worker_lock(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / 'worker.lock').open('a+b') as lock:
        try:
            if os.name == 'nt':
                import msvcrt
                if lock.tell() == 0:
                    lock.write(b'0'); lock.flush()
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise Error('Another worker is using this work directory') from exc
        yield


class Worker:
    def __init__(self, cfg, client):
        self.cfg, self.client = cfg, client
        self.directory = cfg['work_dir']
        self.path, self.stop = self.directory / 'state.json', self.directory / 'STOP'
        self.download_due = 0.0
        self.rpc_calls = self.http_posts = 0
        self.state = json.loads(self.path.read_text()) if self.path.exists() else dict(version=1, queue=[], pending=[])
        if not isinstance(self.state, dict) or self.state.get('version') != 1 or not all(isinstance(self.state.get(k), list) for k in ('queue', 'pending')):
            raise Error('Unrecognized worker state file')
        if any(type(n) is not str or not re.fullmatch(r'[0-9]+', n) or int(n) < 2 for n in self.state['queue']):
            raise Error('Invalid number in saved queue')
        for result in self.state['pending']:
            result['factors'] = checked_factors(result['n'], result['factors'])
            result['n'] = str(int(result['n']))
        reported = self.state.setdefault('reported', [])
        if not isinstance(reported, list) or any(type(n) is not str or not re.fullmatch(r'[0-9]+', n) or int(n) < 2 for n in reported):
            raise Error('Invalid reported-input history')
        limit = cfg.get('reported_limit', 10000)
        # Keep the newest occurrence of each input, bounded across restarts.
        self.state['reported'] = list(reversed(list(dict.fromkeys(reversed(reported)))[:limit]))
        self.reported = set(self.state['reported'])
        queue = self.state['queue']
        self.state['queue'] = list(dict.fromkeys(n for n in queue if n not in self.reported))
        removed = len(queue) - len(self.state['queue'])
        if removed:
            say(f'Removed {removed} duplicate/already-reported queued inputs')
        if removed or reported != self.state['reported']:
            self.save()

    def save(self):
        name = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=self.directory, delete=False) as f:
                name = f.name
                json.dump(self.state, f, indent=2)
                f.write('\n')
            os.replace(name, self.path)
        finally:
            if name and os.path.exists(name):
                os.unlink(name)

    def wait(self, seconds):
        end = time.monotonic() + seconds
        while not self.stop.exists() and time.monotonic() < end:
            time.sleep(min(1.0, max(0, end - time.monotonic())))
        if self.stop.exists():
            raise Stopped()

    def retry(self, function, *args, rpc_calls=1):
        while True:
            self.rpc_calls += rpc_calls
            self.http_posts += 1
            try:
                return function(*args)
            except Retryable as exc:
                say(f'{exc}; retrying in {self.cfg["sleepseconds"]:g}s')
                self.wait(self.cfg['sleepseconds'])

    def next_number(self):
        return next((n for n in self.state['queue'] if self.cfg['min_digits'] <= len(n) <= self.cfg['max_digits']), None)

    def fetch(self):
        delay = self.download_due - time.monotonic()
        if delay > 0:
            self.wait(delay)
        known = set(self.state['queue']) | {r['n'] for r in self.state['pending']} | self.reported
        have = sum(self.cfg['min_digits'] <= len(n) <= self.cfg['max_digits'] for n in self.state['queue'])
        added = 0
        skipped_reported = skipped_duplicate = rejected = 0
        for digits in range(self.cfg['min_digits'], self.cfg['max_digits'] + 1):
            if self.stop.exists() or have >= self.cfg['batch_size']:
                break
            numbers = self.retry(self.client.download, digits, self.cfg['batch_size'] - have)
            for value in numbers:
                try:
                    n = number(value)
                    if len(n) != digits:
                        raise Error('FactorDB returned a work item outside the requested digit size')
                except Error:
                    rejected += 1
                    continue
                if n in self.reported:
                    skipped_reported += 1
                    continue
                if n in known:
                    skipped_duplicate += 1
                    continue
                if have < self.cfg['batch_size']:
                    self.state['queue'].append(n); known.add(n); have += 1; added += 1
            self.state['queue'].sort(key=lambda n: (len(n), n))
            self.save()
        if have < self.cfg['batch_size'] and not self.stop.exists():
            self.download_due = time.monotonic() + self.cfg['short_download_delay']
        bad_items = f'; rejected {rejected} invalid items' if rejected else ''
        say(f'Fetched {added} inputs; skipped {skipped_reported} already-reported and '
            f'{skipped_duplicate} duplicate inputs{bad_items}; {have} queued in the configured range')

    def submit(self):
        pending = self.state['pending']
        if pending:
            count = len(pending)
            say(f'Submitting {count} factorizations '
                f'({self.rpc_calls + count} RPC calls, {self.http_posts + 1} HTTP POSTs including this submission)')
            start = time.monotonic()
            previous_posts = self.http_posts
            self.retry(self.client.submit, pending, rpc_calls=count)
            elapsed = time.monotonic() - start
            attempts = self.http_posts - previous_posts
            acknowledged = {r['n'] for r in pending}
            for n in (r['n'] for r in pending):
                if n not in self.reported:
                    self.state['reported'].append(n)
                    self.reported.add(n)
            self.state['reported'] = self.state['reported'][-self.cfg.get('reported_limit', 10000):]
            self.reported = set(self.state['reported'])
            self.state['queue'] = [n for n in self.state['queue'] if n not in acknowledged]
            self.state['pending'] = []
            self.save()
            retries = f' ({attempts} POST attempts)' if attempts > 1 else ''
            say(f'Submitted {count} factorizations in {duration(elapsed)}{retries}')
            self.rpc_calls = self.http_posts = 0

    def factor_batch(self, executor, slots):
        # Only factoring runs in the pool. The caller owns state and RPCs.
        minimum, maximum = self.cfg['min_digits'], self.cfg['max_digits']
        remaining = iter([n for n in self.state['queue'] if minimum <= len(n) <= maximum])
        batch_time = self.cfg['batch_time']
        start = time.monotonic()
        deadline = start + batch_time if batch_time else math.inf
        idle = slots[:]
        running = {}
        job_times = []
        while True:
            while idle and not self.stop.exists() and time.monotonic() < deadline:
                n = next(remaining, None)
                if n is None:
                    break
                if self.cfg['compact']:
                    factoring_progress(n)
                else:
                    say(f'Factoring C{len(n)}: {n}')
                slot = idle.pop()
                running[executor.submit(timed_factor, slot, n)] = slot, n
            if not running:
                if job_times:
                    elapsed = time.monotonic() - start
                    count = len(job_times)
                    say(f'Factored {count} in {duration(elapsed)} '
                        f'({duration(elapsed / count)}/factor wall, {duration(median(job_times))} median job)')
                return
            completed, _ = wait(running, timeout=0.1, return_when=FIRST_COMPLETED)
            failure = None
            for future in completed:
                slot, n = running.pop(future)
                idle.append(slot)
                try:
                    factors, elapsed = future.result()
                except (Error, OSError, ValueError) as exc:
                    if failure is None:
                        failure = n, exc
                    continue
                self.state['pending'].append(dict(n=n, factors=factors))
                self.state['queue'].remove(n)
                self.save()
                job_times.append(elapsed)
                if not self.cfg['compact']:
                    say(f'{n} = {" * ".join(factors)} ({duration(elapsed)})')
            if failure is not None:
                n, exc = failure
                raise Error(f'Factoring failed for {n}. {exc}') from exc

    def run(self, once=False):
        nslots = self.cfg['threads'] if self.cfg['method'] == 'mpu' else 1
        slots = [FactorSlot(self.cfg) for _ in range(nslots)]
        with ThreadPoolExecutor(max_workers=nslots) as executor:
            try:
                while True:
                    self.submit()
                    if self.stop.exists():
                        raise Stopped()
                    if self.next_number() is None:
                        self.fetch()
                    if self.next_number() is None:
                        if once:
                            return
                        if not self.cfg['short_download_delay']:
                            self.wait(self.cfg['sleepseconds'])
                        continue
                    self.factor_batch(executor, slots)
                    self.submit()
                    if once:
                        return
            except BaseException:
                for slot in slots:
                    slot.close(interrupt=True)
                raise
            finally:
                for slot in slots:
                    slot.close()


def configuration(args):
    path = (args.config or Path('factordb.json')).resolve()
    if args.config and not path.exists():
        raise Error(f'Configuration file does not exist: {path}')
    cfg = dict(api_url='https://factordb.com:4059/rpc', min_digits=90, max_digits=100,
               threads=1, batch_size=10, batch_time=600, sleepseconds=300, short_download_delay=3,
               http_timeout=30, work_dir='factordb-work', fdb_user_token='', reported_limit=10000,
               method='msiqs', exec_msiqs='', exec_yafu='', exec_mpu='')
    if path.exists():
        supplied = json.loads(path.read_text())
        if not isinstance(supplied, dict):
            raise Error('Configuration must be a JSON object')
        cfg.update(supplied)
    for key in ('min_digits', 'max_digits', 'threads', 'method', 'short_download_delay'):
        override = getattr(args, key, None)
        if override is not None:
            cfg[key] = override
    cfg['compact'] = args.compact
    methods = {'msiqs': 'msiqs', 'yafu': 'yafu', 'mpu': 'factordb-batch-worker'}
    method = cfg['method']
    if not isinstance(method, str) or method not in methods:
        raise Error('method must be msiqs, yafu, or mpu')
    cfg['fdb_user_token'] = os.environ.get('FDB_USER_TOKEN') or cfg['fdb_user_token']
    if not isinstance(cfg['fdb_user_token'], str) or not re.fullmatch(r'[0-9a-fA-F]{32}', cfg['fdb_user_token']):
        raise Error('Set fdb_user_token in the config or FDB_USER_TOKEN to your 32-character FactorDB token')
    for key in ('min_digits', 'max_digits', 'threads', 'batch_size', 'reported_limit'):
        if type(cfg[key]) is not int:
            raise Error(f'{key} must be an integer')
    if not 2 <= cfg['min_digits'] <= cfg['max_digits'] or not 1 <= cfg['threads'] <= 256 or not 1 <= cfg['batch_size'] <= 500:
        raise Error('Invalid digit range, thread count (1-256), or batch size (1-500)')
    if cfg['reported_limit'] < 1:
        raise Error('reported_limit must be at least one')
    for key in ('batch_time', 'sleepseconds', 'http_timeout', 'short_download_delay'):
        minimum = 1 if key in ('sleepseconds', 'http_timeout') else 0
        if type(cfg[key]) not in (int, float) or not math.isfinite(cfg[key]) or cfg[key] < minimum:
            raise Error(f'Invalid {key}')
    if cfg.get('sleep_during') or cfg.get('pause_while_running'):
        raise Error('sleep_during and pause_while_running are not supported by this worker; use its STOP file instead')
    cfg['work_dir'] = (path.parent / cfg['work_dir']).resolve()
    if args.command == 'run':
        key = f'exec_{method}'
        executable = cfg[key]
        override = args.msiqs if method == 'msiqs' else None
        if method == 'msiqs':
            executable = override or executable or cfg.get('msiqs_executable')
        if executable:
            base = Path.cwd() if override else path.parent
            candidate = base / executable
            if os.name == 'nt' and not candidate.suffix and not candidate.is_file():
                candidate = candidate.with_suffix('.exe')
            resolved = str(candidate.resolve()) if candidate.is_file() else shutil.which(executable)
        else:
            name = methods[method] + ('.exe' if os.name == 'nt' else '')
            candidates = [Path.cwd() / name, Path(__file__).resolve().parent / name, Path(__file__).resolve().parents[1] / name]
            resolved = next((str(p.resolve()) for p in candidates if p.is_file()), shutil.which(name))
        if not resolved:
            raise Error(f'Cannot find the {method} executable. Set {key} in the configuration')
        cfg['executable'] = resolved
    return cfg


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs='?', choices=['run', 'fetch', 'submit'], default='run')
    parser.add_argument('--config', type=Path, help='JSON configuration (default: ./factordb.json)')
    parser.add_argument('--compact', action='store_true', help='show digit sizes on a shared progress line')
    parser.add_argument('--method', choices=['msiqs', 'yafu', 'mpu'], help='factor with exec_msiqs, exec_yafu, or exec_mpu')
    parser.add_argument('--msiqs', help='msiqs executable; run mode only')
    parser.add_argument('--short-download-delay', type=float, metavar='SECONDS', help='wait after an underfilled sweep (default: 3)')
    parser.add_argument('--threads', type=int)
    parser.add_argument('--min-digits', type=int)
    parser.add_argument('--max-digits', type=int)
    parser.add_argument('--once', action='store_true', help='run one batch, then exit')
    args = parser.parse_args()
    client = None
    try:
        cfg = configuration(args)
        client = Client(cfg)
        with worker_lock(cfg['work_dir']):
            worker = Worker(cfg, client)
            if args.command == 'run':
                worker.run(args.once)
            else:
                getattr(worker, args.command)()
    except Stopped:
        say('Stopped; remaining work and unsent factors are saved')
    except KeyboardInterrupt:
        say('Interrupted; remaining work and unsent factors are saved')
        return 130
    except (Error, OSError, ValueError, KeyError, TypeError) as exc:
        finish_progress()
        print(f'factordb: {exc}', file=sys.stderr)
        return 1
    finally:
        finish_progress()
        if client is not None:
            client.close()
    return 0


def interrupt(signum, frame):
    raise KeyboardInterrupt()


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, interrupt)
    sys.exit(main())
