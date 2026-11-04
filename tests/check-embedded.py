#!/usr/bin/env python3
"""Build the frozen editions independently; artifacts stay in a temporary tree."""
from pathlib import Path
import os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
cc=shlex.split(os.environ.get('CC','cc'))
flags=shlex.split(os.environ.get('CPPFLAGS',''))+shlex.split(os.environ.get('CFLAGS','-O2 -std=c99 -Wall -Wextra'))
ldflags=shlex.split(os.environ.get('LDFLAGS',''))
with tempfile.TemporaryDirectory(prefix='msiqs-embedded-') as tmp:
    def test(name,sources,libs=(),extra=()):
        binary=str(Path(tmp)/name)
        subprocess.run(cc+flags+list(extra)+['-o',binary]+sources+ldflags+list(libs),cwd=ROOT,check=True)
        subprocess.run([binary],cwd=ROOT,check=True)
    test('tiny',['tests/tiny-check.c','embedded/tinysiqs128.c'],['-lm'])
    test('tiny-no128',['tests/tiny-check.c','embedded/tinysiqs128.c'],['-lm'],['-DHAVE_UINT128=0'])
    test('native128',['tests/native128-check.c'],['-lgmp','-lm'])
    sources=['tests/embedded-check.c','embedded/lanczos.c','embedded/pr32_iter.c','embedded/tinyecm64.c']
    test('serial',sources,['-lgmp','-lm'])
    test('serial-no128',sources,['-lgmp','-lm'],['-DHAVE_UINT128=0'])
print('Embedded checks: all enabled checks passed.')
