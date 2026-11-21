#!/usr/bin/env python3
"""Cheap command-line checks for automatic and forced LP policy selection."""
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
    print(f'{passed}/{passed} LP command-line checks passed.')


if __name__ == '__main__':
    main()
