#!/usr/bin/env python3
"""Offline checks for the FactorDB worker and its executable protocols."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import factordb


FACTORS = {'77': ['7', '11'], '91': ['7', '13'], '143': ['11', '13'],
           '169': ['13', '13'], '1001': ['7', '11', '13']}
BACKEND = r"""
import os
from pathlib import Path
import sys

factors = {'77': '7 11', '91': '7 13', '143': '11 13',
           '169': '13 13', '1001': '7 11 13'}
assert 'FDB_USER_TOKEN' not in os.environ
method = Path(sys.argv[0]).stem
with open(os.environ['FACTORDB_TEST_TRACE'], 'a') as trace:
    print(method, file=trace)
if method == 'msiqs':
    assert sys.argv[1:3] == ['-t', '2']
    n = sys.argv[3]
    print(n + ': ' + factors[n])
elif method == 'yafu':
    expr = sys.stdin.readline().strip()
    assert sys.argv[1:] == [expr, '-threads', '2']
    assert expr.startswith('factor(') and expr.endswith(')')
    n = expr[7:-1]
    print('***factors found***')
    for p in factors[n].split():
        print('P' + str(len(p)) + ' = ' + p)
else:
    assert sys.argv[1:] == ['--mode', 'factor']
    for line in sys.stdin:
        job, n = line.strip().split('\t')
        assert job == 'factor'
        print('ok', n, factors[n], 'p' * len(factors[n].split()), sep='\t', flush=True)
"""


class WorkerTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.base = Path(directory.name)
        self.trace = self.base / 'trace'
        self.config_path = self.base / 'factordb.json'
        for method in ('msiqs', 'yafu', 'mpu'):
            (self.base / f'{method}.py').write_text(BACKEND, encoding='utf-8')
        self.output = io.StringIO()
        capture = redirect_stdout(self.output)
        capture.__enter__()
        self.addCleanup(capture.__exit__, None, None, None)
        self.addCleanup(factordb.finish_progress)

        environment = patch.dict(os.environ, {'FDB_USER_TOKEN': '0' * 32,
                                             'FACTORDB_TEST_TRACE': str(self.trace)})
        environment.start()
        self.addCleanup(environment.stop)

        # Use the Python interpreter to launch the stand-ins on every platform.
        popen = subprocess.Popen
        launch = patch.object(factordb.subprocess, 'Popen',
                              lambda argv, **kwargs: popen([sys.executable, *argv], **kwargs))
        launch.start()
        self.addCleanup(launch.stop)

    def configuration(self, method='msiqs', command='run', **overrides):
        saved = dict(min_digits=2, max_digits=3, batch_size=5, threads=2,
                     exec_msiqs='./msiqs.py', exec_yafu='./yafu.py', exec_mpu='./mpu.py',
                     work_dir='work', sleepseconds=9, _comments={'threads': 'Test workers'})
        saved.update(overrides)
        self.config_path.write_text(json.dumps(saved), encoding='utf-8')
        args = SimpleNamespace(config=self.config_path, command=command, method=method,
                               compact=True, msiqs=None)
        cfg = factordb.configuration(args)
        cfg['work_dir'].mkdir(exist_ok=True)
        return cfg

    def test_download_requests_random_inputs_for_exact_digit_count(self):
        client = factordb.Client(self.configuration(command='fetch'))

        def exchange(payload):
            self.assertEqual(payload['method'], 'download')
            self.assertEqual(payload['params'],
                             dict(table='C', digits=2, count=5, random=True, terms=False))
            return {payload['id']: {'numbers': ['77']}}

        client.exchange = exchange
        self.assertEqual(client.download(2, 5), ['77'])

    def test_submit_reports_factors_in_one_rpc_batch(self):
        client = factordb.Client(self.configuration(command='submit'))
        results = [{'n': n, 'factors': FACTORS[n]} for n in ('77', '169')]
        with patch.object(client, 'exchange', return_value={1: None, 2: None}) as exchange:
            client.submit(results)

        exchange.assert_called_once_with([
            dict(jsonrpc='2.0', id=1, method='report_factors',
                 params=dict(target={'expr': '77'}, factors=['7', '11'], credit=True)),
            dict(jsonrpc='2.0', id=2, method='report_factors',
                 params=dict(target={'expr': '169'}, factors=['13', '13'], credit=True)),
        ])

    def test_serial_msiqs_does_not_pass_thread_option(self):
        slot = factordb.FactorSlot(self.configuration(threads=1))
        with patch.object(slot, '_run_once', return_value='77: 7 11\n') as run:
            self.assertEqual(slot.factor('77'), FACTORS['77'])
        run.assert_called_once_with([slot.executable, '77'])

    def test_yafu_uses_installation_ini_in_isolated_working_directory(self):
        slot = factordb.FactorSlot(self.configuration('yafu'))
        ini = self.base / 'yafu.ini'
        source = 'threads=4\nggnfs_dir=relative nfs/\nB1ecm=2000\n'
        ini.write_text(source, encoding='utf-8')

        def run(command, *, data, cwd):
            self.assertNotEqual(Path(cwd).resolve(), self.base.resolve())
            staged = (Path(cwd) / 'yafu.ini').read_text(encoding='utf-8')
            tool_path = os.path.join(self.base.resolve(), 'relative nfs/')
            self.assertEqual(staged, source.replace('relative nfs/', tool_path))
            return '***factors found***\nP1 = 7\nP2 = 11\n'

        with patch.object(slot, '_run_once', side_effect=run):
            self.assertEqual(slot.factor('77'), FACTORS['77'])
        self.assertEqual(ini.read_text(encoding='utf-8'), source)

    def test_each_backend_factors_a_batch_from_successive_sizes(self):
        for method in ('msiqs', 'yafu', 'mpu'):
            with self.subTest(method=method):
                self.output.seek(0)
                self.output.truncate()
                self.trace.write_text('', encoding='utf-8')
                calls = []

                def download(digits, count):
                    calls.append(('download', digits, count))
                    return {2: ['91', '77', '7*11'], 3: ['169', '143'], 4: ['1001']}[digits]

                def submit(results):
                    calls.append(('submit', {r['n']: r['factors'] for r in results}))

                cfg = self.configuration(method, max_digits=4, work_dir=method)
                worker = factordb.Worker(cfg, SimpleNamespace(download=download, submit=submit))
                worker.run(once=True)
                self.assertEqual(calls, [
                    ('download', 2, 5), ('download', 3, 3), ('download', 4, 1),
                    ('submit', FACTORS),
                ])
                self.assertEqual(worker.state['queue'], [])
                self.assertEqual(worker.state['pending'], [])
                self.assertCountEqual(worker.state['reported'], FACTORS.keys())
                output = self.output.getvalue()
                self.assertIn('Factoring C2 C2 C3 C3 C4\n', output)
                self.assertIn('Fetched 5 inputs; skipped 0 already-reported and 1 duplicate inputs', output)
                self.assertIn('Factored 5', output)
                self.assertIn('Submitting 5 factorizations (8 RPC calls, 4 HTTP POSTs', output)
                self.assertIn('Submitted 5', output)
                self.assertNotIn('\n\n', output)
                self.assertNotIn('77 =', output)
                self.assertEqual(self.trace.read_text().splitlines(),
                                 [method] * (2 if method == 'mpu' else 5))

    def test_underfilled_batch_delays_the_next_sweep(self):
        now = 10.0
        calls = []

        def wait(seconds):
            nonlocal now
            now += seconds

        def download(digits, count):
            calls.append(('download', digits, count, now))
            if len(calls) == 1:
                return ['77']
            if len(calls) > 2:
                worker.stop.touch()
            return []

        def submit(results):
            calls.append(('submit', len(results), now))

        worker = factordb.Worker(self.configuration(), SimpleNamespace(download=download, submit=submit))
        with patch.object(factordb.time, 'monotonic', side_effect=lambda: now), \
             patch.object(worker, 'wait', side_effect=wait) as mock_wait, \
             self.assertRaises(factordb.Stopped):
            worker.run()
        mock_wait.assert_called_once_with(3)
        self.assertEqual(calls, [
            ('download', 2, 5, 10.0), ('download', 3, 4, 10.0),
            ('submit', 1, 10.0), ('download', 2, 5, 13.0),
        ])

    def test_fetch_saves_valid_items_around_bad_download_items(self):
        download = Mock(return_value=['77', 'unsupported(5)', '143', '91'])
        worker = factordb.Worker(self.configuration(max_digits=2), SimpleNamespace(download=download))
        worker.fetch()
        self.assertEqual(worker.state['queue'], ['77', '91'])
        self.assertEqual(json.loads(worker.path.read_text())['queue'], ['77', '91'])
        self.assertIn('rejected 2 invalid items', self.output.getvalue())

    def test_submission_retries_saved_results_and_counts_post_attempts(self):
        submit = Mock(side_effect=[factordb.Retryable('temporary failure'), None])
        client = SimpleNamespace(download=lambda digits, count: ['77'], submit=submit)
        worker = factordb.Worker(self.configuration(max_digits=2), client)
        pending = [{'n': '77', 'factors': FACTORS['77']}]

        def wait(seconds):
            self.assertEqual(seconds, 9)
            self.assertEqual(json.loads(worker.path.read_text())['pending'], pending)

        with patch.object(worker, 'wait', side_effect=wait) as mock_wait:
            worker.run(once=True)
        mock_wait.assert_called_once()
        self.assertEqual(submit.call_count, 2)
        for call in submit.call_args_list:
            self.assertEqual(call.args, (pending,))
        self.assertIn('Submitting 1 factorizations (2 RPC calls, 2 HTTP POSTs', self.output.getvalue())
        self.assertIn('(2 POST attempts)', self.output.getvalue())
        self.assertEqual(worker.state['pending'], [])
        self.assertEqual(worker.state['reported'], ['77'])

    def test_factor_failure_keeps_input_and_saves_pending_results(self):
        client = SimpleNamespace(download=lambda digits, count: ['77', '99'] if digits == 2 else [],
                                 submit=Mock())
        worker = factordb.Worker(self.configuration(), client)
        with self.assertRaisesRegex(factordb.Error, '99'):
            worker.run(once=True)
        saved = json.loads(worker.path.read_text())
        self.assertEqual(saved['queue'], ['99'])
        self.assertEqual(saved['pending'], [{'n': '77', 'factors': FACTORS['77']}])
        client.submit.assert_not_called()

    def test_configuration_only_resolves_the_chosen_backend_in_run_mode(self):
        cfg = self.configuration('yafu', exec_msiqs='missing/msiqs', exec_mpu='missing/mpu')
        self.assertEqual(Path(cfg['executable']), (self.base / 'yafu.py').resolve())
        cfg = self.configuration('mpu', command='submit', exec_mpu='missing/mpu')
        self.assertNotIn('executable', cfg)

    def test_windows_suffix_for_explicit_executable_path(self):
        executable = self.base / 'factordb-batch-worker.exe'
        executable.touch()
        with patch.object(factordb, 'os', SimpleNamespace(name='nt', environ=os.environ)):
            cfg = self.configuration('mpu', exec_mpu='./factordb-batch-worker')
        self.assertEqual(Path(cfg['executable']), executable.resolve())


if __name__ == '__main__':
    unittest.main()
