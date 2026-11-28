#!/usr/bin/env python3
"""Full 270-bit TLP factorization, interruption, replay and policy isolation."""
import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from _support import EXE, RESULTS, TOOL

N = 1148829108227602429702806949260709092933481257528975829355909861594747665058507777
P = 27062981639101571779863334701788273295629
Q = 42450204620755190797397639599411846697413
# Completed journal for 15 produced by msiqs at 82d4d75 with --journal FILE 15.
V1_JOURNAL = (
    b'MPUSIQJ\nVersion: 1\nByte-order: little-endian\nN: 15\n--\n'
    + bytes.fromhex(
        '4d50555349514a0001000000040302014a424c4b010000000000000000000000'
        '05000000010000000000000000000000db612e23cc6006000000000000000000'
        '0118a89b8f20d64b010000000f4a424c4b050000000100000000000000130000'
        '00010000000000000000000000a1622e23cc600600c600000000000000e43f3b'
        '4e8367f834010000000f02000000010000000301000000054a424c4b06000000'
        '020000000000000017000000010000000000000000000000b2622e23cc600600'
        'd70000000000000039668b14a0fb51c701000000010000000f02000000010000'
        '00030100000005'
    )
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=str, default=str(EXE))
    parser.add_argument('--threads', type=int,
                        help='worker count (default: 4 with pthreads, 1 otherwise)')
    args = parser.parse_args()
    if args.threads is not None and not 1 <= args.threads <= 256:
        parser.error('--threads must be between 1 and 256')
    help_result = subprocess.run([args.executable, '--help'], capture_output=True,
                                 text=True, check=True, timeout=10)
    thread_args = []
    if '--threads' in help_result.stdout:
        thread_args = ['-t', str(args.threads if args.threads is not None else 4)]
    elif args.threads not in (None, 1):
        parser.error('--threads above 1 requires a pthread build')
    module = importlib.util.spec_from_file_location('journal_checks',
               __file__.replace('test-tlp.py', 'test-journal.py'))
    checks = importlib.util.module_from_spec(module)
    module.loader.exec_module(checks)
    path = RESULTS / 'tlp-interrupted.journal'
    assert not path.exists(), 'use a fresh SIQS_JOURNAL_TEST_DIR'
    command = [args.executable, '--3lp', *thread_args, '-vvv', '--journal', str(path)]
    print('TLP integration artifacts:', RESULTS, flush=True)
    with (RESULTS / 'tlp-interrupted.log').open('w') as log:
        process = subprocess.Popen(command + [str(N)], stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and process.poll() is None:
                if path.exists():
                    data = path.read_bytes()
                    batches = checks.frames(data, partial=True)
                    if any(kind == 3 and count > 50 for _, _, kind, count in batches):
                        break
                time.sleep(0.05)
            else:
                raise AssertionError('no committed relation batch before interruption')
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=15)
    assert process.returncode != 0
    saved = path.read_bytes()
    mismatch = subprocess.run([args.executable, '--2lp', *thread_args, '--journal', str(path)],
                              capture_output=True, text=True, timeout=60)
    (RESULTS / 'tlp-policy-mismatch.log').write_text(mismatch.stdout + mismatch.stderr)
    assert mismatch.returncode != 0
    assert 'resolved parameters, factor base, or attempt differs' in mismatch.stderr
    assert path.read_bytes() == saved, 'policy mismatch changed the journal'
    print('PASS interrupted TLP rejects ordinary-policy restart without writing', flush=True)

    with (RESULTS / 'tlp-resumed.log').open('w') as log:
        resumed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    assert resumed.returncode == 0
    output = (RESULTS / 'tlp-resumed.log').read_text()
    assert f'{N}: {P} {Q}' in output
    assert '3-LP' in output and '3lp_' in output
    assert any(', pending ' in line for line in output.splitlines() if line.startswith('# siqs relations '))
    assert '# siqs 3LP batches:' in output
    assert '# siqs matrix:' in output and ', 0 LP rows' in output
    assert f'# siqs resuming journal {path}' in output
    assert any(line.startswith('# siqs solve: ') for line in output.splitlines())
    print('PASS resumed TLP factors the input through an LP-free matrix', flush=True)

    inspected = subprocess.run([sys.executable, str(TOOL), '--json', '--verify-relations',
                                str(path)], capture_output=True, text=True, timeout=180)
    (RESULTS / 'tlp-info.json').write_text(inspected.stdout)
    (RESULTS / 'tlp-info.stderr').write_text(inspected.stderr)
    assert inspected.returncode == 0, inspected.stderr
    info = json.loads(inspected.stdout)
    ctx = info['contexts'][0]
    snapshot = ctx['snapshot']
    assert ctx['parameters']['max_large_primes'] == 3
    assert info['relations']['three_lp'] > 0
    assert info['relation_congruences_verified'] == info['relation_count']
    assert 0 < snapshot['full_relations'] < snapshot['stored_relations']
    assert sum(snapshot[key] for key in snapshot if key.startswith('length_')) == snapshot['full_relations']
    assert info['job_end']['complete'] == 1
    assert len(info['runs']) >= 2
    print('PASS all saved congruences, triples and useful-relation counters verify', flush=True)

    complete = path.read_bytes()
    replay = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (RESULTS / 'tlp-completed-replay.log').write_text(replay.stdout + replay.stderr)
    assert replay.returncode == 0 and f'{N}: {P} {Q}' in replay.stdout
    assert path.read_bytes() == complete
    print('PASS completed replay leaves every journal byte unchanged', flush=True)

    old = RESULTS / 'unsupported-v1.journal'
    old.write_bytes(V1_JOURNAL)
    rejected = subprocess.run([args.executable, '--journal', str(old)],
                              capture_output=True, text=True, timeout=10)
    assert rejected.returncode != 0
    assert 'unsupported journal format version' in rejected.stderr
    assert old.read_bytes() == V1_JOURNAL
    (RESULTS / 'unsupported-v1.log').write_text(rejected.stdout + rejected.stderr)
    print('PASS release-bound version rejection leaves the old journal unchanged', flush=True)

    small = subprocess.run([args.executable, '--3lp', '-v', '1000000016000000063'],
                           capture_output=True, text=True, timeout=30)
    assert small.returncode == 0 and '1000000007 1000000009' in small.stdout
    assert '3lp_' not in small.stdout and '3-LP' not in small.stdout
    (RESULTS / 'tlp-uncovered-fallback.log').write_text(small.stdout + small.stderr)
    (RESULTS / 'tlp-proof.json').write_text(json.dumps(dict(
        n=str(N), factors=[str(P), str(Q)], completed_replay_byte_identical=True,
        sha256=hashlib.sha256(complete).hexdigest(), snapshot=snapshot,
        relations=info['relations'], verified=info['relation_congruences_verified']), indent=2) + '\n')
    print('PASS uncovered size uses ordinary policy; all TLP integration checks passed', flush=True)


if __name__ == '__main__':
    main()
