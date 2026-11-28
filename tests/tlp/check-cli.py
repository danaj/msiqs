#!/usr/bin/env python3
"""Command-line checks for LP policy selection and progress reporting."""
import argparse
from pathlib import Path
import subprocess


N = '1000000016000000063'
FACTORIZATION = N + ': 1000000007 1000000009'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', default=str(Path(__file__).resolve().parents[2] / 'msiqs'))
    args = parser.parse_args()
    passed = 0

    def run(*arguments):
        return subprocess.run([args.executable, *arguments], capture_output=True,
                              text=True, timeout=30)

    help_result = run('--help')
    assert help_result.returncode == 0, help_result.stderr
    assert '--2lp' in help_result.stdout and '--3lp' in help_result.stdout
    assert '--tlp' not in help_result.stdout
    assert '361' in help_result.stdout
    passed += 1
    print('PASS help describes the automatic boundary and explicit LP overrides')

    removed = run('--tlp', N)
    assert removed.returncode != 0, 'removed --tlp option was accepted'
    assert '--tlp' in removed.stderr
    passed += 1
    print('PASS removed --tlp option is rejected')

    for flags in [('--2lp', '--3lp'), ('--3lp', '--2lp')]:
        conflict = run(*flags, N)
        assert conflict.returncode != 0, 'conflicting overrides were accepted'
        assert '--2lp' in conflict.stderr and '--3lp' in conflict.stderr
        passed += 1
    print('PASS conflicting LP overrides are rejected in either order')

    for flags in [(), ('--2lp',), ('--3lp',), ('--2lp', '--2lp'), ('--3lp', '--3lp')]:
        small = run(*flags, '-v', N)
        assert small.returncode == 0, small.stdout + small.stderr
        assert FACTORIZATION in small.stdout
        assert '3lp_' not in small.stdout and '3-LP' not in small.stdout
        passed += 1
    print('PASS small input uses supported ordinary policies in all three modes')

    example = '22095311209999409685885162322219'
    quiet = run(example)
    summary = run('-v', example)
    verbose = run('-vv', example)
    assert quiet.returncode == summary.returncode == verbose.returncode == 0
    assert quiet.stdout.splitlines() == [verbose.stdout.splitlines()[-1]]
    for result in (summary, verbose):
        header = next(line for line in result.stdout.splitlines() if line.startswith('# siqs 1-LP, policy ['))
        assert all(f', {label} ' in header for label in ('q', 'FB', 'M'))
        parameters = next(line for line in result.stdout.splitlines() if line.startswith('# siqs mult '))
        assert all(f', {label} ' in parameters for label in ('d', 'LP', 'R', 'pmin', 'pmax'))
        assert '# siqs policy one_lp_' not in result.stdout
        totals = next(line for line in result.stdout.splitlines() if line.startswith('# siqs used '))
        assert all(label in totals for label in ('families', 'polynomials', 'candidates', 'LP-free relations'))
        assert '# siqs wall time ' in result.stdout
    assert '#' in verbose.stdout.splitlines()
    assert '# siqs relations ' in verbose.stdout and 'LP-free' in verbose.stdout
    assert '# siqs BIDS:' not in verbose.stdout
    passed += 1
    print('PASS quiet factors only, with a compact timed collection summary at -vv')

    p = 2**127 - 1
    q = 1329227995784915872903807060280344457
    n = p * q
    threads = ['-t', '4'] if '--threads' in help_result.stdout else []
    ordinary = subprocess.run([args.executable, '--2lp', '-vvv', *threads, str(n)],
                              capture_output=True, text=True, timeout=120)
    assert ordinary.returncode == 0, ordinary.stdout[-3000:] + ordinary.stderr
    assert '2-LP' in ordinary.stdout and f'{n}: {q} {p}' in ordinary.stdout
    assert '# siqs policy two_lp_' in ordinary.stdout
    progress = [line for line in ordinary.stdout.splitlines() if line.startswith('# siqs relations ')]
    assert progress and all(', pending ' not in line for line in progress), ordinary.stdout[-3000:]
    passed += 1
    print('PASS 2LP progress omits the pending queue at diagnostic verbosity')

    readiness_prefix = '# siqs matrix readiness:'
    readiness_start = '# siqs starting matrix readiness checks'
    assert readiness_prefix not in ordinary.stdout
    assert ordinary.stdout.splitlines().count(readiness_start) == 1
    trace = subprocess.run([args.executable, '--2lp', '-vvvv', *threads, str(n)],
                           capture_output=True, text=True, timeout=120)
    assert trace.returncode == 0, trace.stdout[-3000:] + trace.stderr
    assert f'{n}: {q} {p}' in trace.stdout
    lines = trace.stdout.splitlines()
    assert lines.count(readiness_start) == 1
    checks = [line for line in lines if line.startswith(readiness_prefix)]
    assert checks and all('LP-free columns, need ' in line and 'FB/sign rows plus ' in line
                          for line in checks), trace.stdout[-3000:]
    assert lines.index(readiness_start) < lines.index(checks[0])
    passed += 1
    print('PASS one matrix readiness announcement at -vvv and individual checks at -vvvv')
    print(f'{passed}/{passed} LP command-line checks passed.')


if __name__ == '__main__':
    main()
