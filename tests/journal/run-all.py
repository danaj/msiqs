#!/usr/bin/env python3
"""Build both modes and run journal, recovery, and existing SIQS checks."""
from pathlib import Path
import json
import struct
import zlib
import subprocess
import sys
import os
sys.dont_write_bytecode = True

from _support import ROOT, BUILD, RESULTS
FIXTURES = Path(__file__).resolve().parent
EXE = RESULTS/'msiqs'
os.environ['SIQS_JOURNAL_TEST_DIR'] = str(RESULTS)
os.environ['SIQS_JOURNAL_TEST_EXECUTABLE'] = str(EXE)
SOURCES = ['msiqs.c','siqs.c','lanczos.c','pr32_iter.c','tinyecm64.c']


def step(name, command, cwd=ROOT):
    print('START', name, flush=True)
    with (RESULTS/(name+'.log')).open('w') as log:
        p = subprocess.run(command,cwd=cwd,stdout=log,stderr=subprocess.STDOUT)
    if p.returncode:
        print('FAILED',name,'see',RESULTS/(name+'.log'),flush=True)
        raise SystemExit(p.returncode)
    print('PASS',name,flush=True)


def splitter_counter_compatibility(data, proof):
    # Version 1 stores SQUFOF and ECM in words 6 and 7 of the 24-word snapshot.
    # The latter word was the always-zero rho counter in historical journals.
    assert proof['split_ecm'] > 0
    for mode in ('legacy', 'mixed'):
        changed = bytearray(data)
        at = changed.index(b'\n--\n') + 20
        while at < len(changed):
            fields = struct.unpack_from('<IIQIIQQQII', changed, at)
            end = at + 56 + fields[3]
            if fields[1] == 3:
                progress = list(struct.unpack_from('<24Q', changed, at + 56))
                progress[3] += 7
                progress[6] += 7
                if mode == 'legacy':
                    progress[6] += progress[7]
                    progress[7] = 0
                struct.pack_into('<24Q', changed, at + 56, *progress)
                crc = zlib.crc32(changed[at + 56:end], zlib.crc32(changed[at:at + 52]))
                struct.pack_into('<I', changed, at + 52, crc)
            at = end
        assert at == len(changed)
        path = RESULTS / ('graph-' + mode + '-counters.journal')
        path.write_bytes(changed)
        name = 'graph-' + mode + '-counters'
        step(name, [str(RESULTS / 'journal-graph'), str(path)])
        lines = (RESULTS / (name + '.log')).read_text().splitlines()
        replay = json.loads(next(line for line in lines if line.startswith('{')))
        expected = dict(proof)
        expected['split_squfof'] += 7
        if mode == 'legacy':
            expected['split_squfof'] += expected['split_ecm']
            expected['split_ecm'] = 0
        assert replay == expected and path.read_bytes() == changed
        step(name + '-info', [sys.executable, str(BUILD / 'tools/journal-info.py'),
                             '--json', '--verify-relations', str(path)])
        summary = json.loads((RESULTS / (name + '-info.log')).read_text())
        counters = summary['contexts'][-1]['snapshot']
        for key in ('split_squfof', 'split_ecm'):
            assert counters[key] == expected[key]
        assert 'split_rho' not in counters


RESULTS.mkdir(exist_ok=True)
print('Journal test artifacts:', RESULTS, flush=True)
step('eta-model',[sys.executable,'-u',str(FIXTURES/'test-eta.py')])
step('build',['perl','tools/build-siqs.pl','--output',str(EXE)]+SOURCES,BUILD)
step('build-serial',['perl','tools/build-siqs.pl','--serial','--output',str(RESULTS/'msiqs-serial'),
                     '--cflags','-O2 -falign-loops=32']+SOURCES,BUILD)
step('journal-tests',[sys.executable,'-u',str(FIXTURES/'test-journal.py'),
                      '--executable',str(EXE),'--serial-executable',str(RESULTS/'msiqs-serial')])
step('build-seeds',['cc','-O1','-o',str(RESULTS/'journal-seeds'),
                    str(FIXTURES/'journal-seeds.c'),str(BUILD/'lanczos.c'),
                    str(BUILD/'pr32_iter.c'),str(ROOT/'tinyecm64.c'),'-lgmp','-lm'])
path=RESULTS/'seeds.journal'
if path.exists():path.unlink()
step('seed-record',[str(RESULTS/'journal-seeds'),str(path)])
data=path.read_bytes()
step('seed-replay',[str(RESULTS/'journal-seeds'),str(path)])
assert path.read_bytes()==data
assert (RESULTS/'seed-record.log').read_bytes()==(RESULTS/'seed-replay.log').read_bytes()
step('seed-info',[sys.executable,str(BUILD/'tools/journal-info.py'),'--json',str(path)])
assert json.loads((RESULTS/'seed-info.log').read_text())['contexts'][0]['initial_rng']==dict(
    lanczos=0x1a6b50f924d38ce7,cofactor=0x8e721c06a9b5d43f,polynomial=0xc3f9821d4076ab5e)
step('build-attempts',['cc','-O1','-o',str(RESULTS/'journal-attempts'),
                       str(FIXTURES/'journal-attempts.c'),str(BUILD/'lanczos.c'),
                       str(BUILD/'pr32_iter.c'),str(ROOT/'tinyecm64.c'),'-lgmp','-lm'])
step('event-headers',[sys.executable,'-u',str(FIXTURES/'test-event-headers.py'),str(RESULTS/'journal-attempts')])
step('solve-flush',[sys.executable,'-u',str(FIXTURES/'test-solve-flush.py'),str(RESULTS/'journal-attempts')])
step('run-clock',[sys.executable,'-u',str(FIXTURES/'test-journal-timing.py'),str(RESULTS/'journal-attempts')])
step('codecs',[str(RESULTS/'journal-attempts'),'--codecs'])
path=RESULTS/'attempts.journal'
if path.exists():path.unlink()
step('attempt-record',[str(RESULTS/'journal-attempts'),str(path)])
data=path.read_bytes()
step('attempt-replay',[str(RESULTS/'journal-attempts'),str(path)])
assert path.read_bytes()==data
step('attempt-info',[sys.executable,str(BUILD/'tools/journal-info.py'),'--json',str(path)])
step('build-graph',['cc','-O2','-DPSIQS','-pthread',
                    '-o',str(RESULTS/'journal-graph'),str(FIXTURES/'journal-graph.c'),
                    str(BUILD/'lanczos.c'),str(BUILD/'pr32_iter.c'),
                    str(ROOT/'tinyecm64.c'),'-lgmp','-lm'])
path=RESULTS/'graph.journal'
if path.exists():path.unlink()
proof=[]
for mode in ('record','replay'):
    step('graph-'+mode,[str(RESULTS/'journal-graph'),str(path)])
    lines=(RESULTS/('graph-'+mode+'.log')).read_text().splitlines()
    proof.append(json.loads(next(line for line in lines if line.startswith('{'))))
assert proof[0]==proof[1]
step('graph-info',[sys.executable,str(BUILD/'tools/journal-info.py'),'--json','--verify-relations',str(path)])
splitter_counter_compatibility(path.read_bytes(), proof[0])
data=path.read_bytes(); at=data.index(b'\n--\n')+4+16
while at<len(data):
    _,kind,_,length,_,_,_,_,_,_=struct.unpack_from('<IIQIIQQQII',data,at)
    if kind==4:break
    at+=56+length
prefix=RESULTS/'graph-without-result.journal';prefix.write_bytes(data[:at])
n='57896044618658097711785492504343953899242261795684735710927136105699223797919'
step('graph-solve',[str(EXE),'-v','-t','1','--journal',str(prefix),n])
assert '170141183460469231731687303715884105727 340282366920938463463374607431768211297' in (RESULTS/'graph-solve.log').read_text()
(RESULTS/'graph-proof.json').write_text(json.dumps({'live':proof[0],'replayed':proof[1],
    'solved_without_saved_partition':True},indent=2)+'\n')
path=RESULTS/'wide-header.journal'
if path.exists():path.unlink()
step('wide-header-record',[str(RESULTS/'journal-attempts'),str(path),'--wide'])
data=path.read_bytes()
step('wide-header-replay',[str(RESULTS/'journal-attempts'),str(path),'--wide'])
assert path.read_bytes()==data
step('wide-header-info',[sys.executable,str(BUILD/'tools/journal-info.py'),'--json',str(path)])
step('existing-check-siqs',['make','check'],BUILD)
step('journal-info',[sys.executable,'-u',str(FIXTURES/'test-journal-info.py')])
print('All journal, recovery and existing SIQS checks passed.',flush=True)
