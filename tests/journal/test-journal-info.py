#!/usr/bin/env python3
"""Inspector checks using real journals, deliberate corruption and live writers."""
import importlib.util,json,signal,struct,subprocess,sys,tempfile,time,zlib
from pathlib import Path
sys.dont_write_bytecode=True
from _support import ROOT, RESULTS, EXE, TOOL
spec=importlib.util.spec_from_file_location('journal_info',TOOL)
info=importlib.util.module_from_spec(spec);spec.loader.exec_module(info)
ONE=((1<<89)-1)*((1<<107)-1);TWO=((1<<127)-1)*((1<<128)-159);passed=0

def check(name):
 global passed
 passed+=1;print('PASS '+name,flush=True)

def inspect(path,verify=False,eta_only=False):
 reader=info.Journal(path,verify,eta_only=eta_only)
 try:reader.scan();return reader.summary(300)
 finally:reader.close()

def cli(path,*args,ok=True):
 p=subprocess.run([sys.executable,str(TOOL),*args,str(path)],capture_output=True,text=True,timeout=60)
 assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
 return p

def frames(data):
 at=data.index(b'\n--\n')+20;result=[]
 while at<len(data):
  fields=list(info.FRAME.unpack_from(data,at));end=at+info.FRAME.size+fields[3]
  result.append((at,end,fields));at=end
 return result

def rewrite(data,frame,changes=None,payload=None):
 start,end,fields=frame;fields=fields.copy()
 for key,value in (changes or {}).items():fields[key]=value
 if payload is None:payload=data[start+info.FRAME.size:end]
 fields[3]=len(payload);fields[-2:]=[0,0]
 header=bytearray(info.FRAME.pack(*fields))
 struct.pack_into('<I',header,48,zlib.crc32(header[:48]))
 struct.pack_into('<I',header,52,zlib.crc32(payload,zlib.crc32(header[:52])))
 return data[:start]+header+payload+data[end:]

def metadata_frame(prefix,kind,payload,count=1,delay_us=0,next_run=False):
 previous=frames(prefix)[-1]
 fields=previous[2]
 elapsed=delay_us if next_run else fields[7]+delay_us
 changes={1:kind,2:fields[2]+1,4:count,5:fields[5]+int(next_run),6:fields[6]+delay_us,7:elapsed}
 return rewrite(prefix,previous,changes,payload)[previous[0]:]

with tempfile.TemporaryDirectory(prefix='journal-info-',dir=RESULTS) as directory:
 tmp=Path(directory);path=tmp/'complete.journal'
 subprocess.run([str(EXE),'-t','1','--journal',str(path),str(ONE)],stdout=subprocess.DEVNULL,check=True)
 data=path.read_bytes();ff=frames(data);batches=[v for v in ff if v[2][1]==3]
 result=inspect(path,True)
 assert result['relation_count']==result['relation_congruences_verified']>0
 assert set(result['contexts'][0]['initial_rng'])=={'lanczos','cofactor','polynomial'}
 assert len(result['runs'])==1 and result['runs'][0]['id']==0
 assert sum(result['byte_breakdown'].values())==len(data) and path.read_bytes()==data
 check('inspection, exact byte accounting and full mathematical verification')
 assert json.loads(cli(path,'--json','--verify-relations').stdout)['relation_count']==result['relation_count']
 text=cli(path,'--eta').stdout
 assert 'N: '+str(ONE) in text and ' full, ' in text and 'Current target:' in text
 assert 'ETA:' not in text and text.rstrip().endswith('Collection completed.')
 assert result['job_end']['complete'] and result['job_end']['partition']['n']==str(ONE)
 check('JSON/text parameters, compact final counters and completion')
 assert f"Stored raw relations: {result['relation_count']:,}; frame checksums and progress OK" in text
 event_frame=next(frame for frame in batches if frame[2][4])
 start,end,_=event_frame;event_offset=info.FRAME.size+8*len(info.PROGRESS)
 altered=bytearray(data);altered[start+event_offset]=255
 bad=tmp/'bad-event.journal';bad.write_bytes(altered)
 assert 'frame checksum mismatch' in cli(bad,'--eta',ok=False).stderr
 bad.write_bytes(rewrite(data,event_frame,payload=altered[start+info.FRAME.size:end]))
 assert 'Collection completed.' in cli(bad,'--eta').stdout
 for flag in ('--json','--strict','--verify-relations'):
  assert 'unknown batch event' in cli(bad,'--eta',flag,ok=False).stderr
 check('compact ETA skips event decoding while retaining frame checksums')
 assert cli(path,'--follow','--eta','--interval','.01').stdout.rstrip().endswith('Collection completed.')
 assert json.loads(cli(path,'--follow','--json','--interval','.01').stdout)['job_end']['complete']
 check('text and JSON followers exit on the final job record')
 stream=tmp/'stream.journal';stream.write_bytes(data[:batches[0][1]])
 with (tmp/'progress.log').open('w') as output:
  follower=subprocess.Popen([sys.executable,str(TOOL),'--follow','--eta','--interval','.01',str(stream)],stdout=output,stderr=output)
  try:
   def await_text(fragment):
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
     content=(tmp/'progress.log').read_text()
     if fragment(content):return content
     assert follower.poll() is None,content
     time.sleep(.01)
    raise AssertionError('missing progress output')
   await_text(lambda content:'ETA:' in content)
   with stream.open('ab') as out:out.write(data[batches[0][1]:batches[1][1]])
   await_text(lambda content:content.count('ETA:')>=2)
   final=ff[-1];assert final[2][1]==6
   with stream.open('ab') as out:out.write(data[batches[1][1]:final[0]])
   await_text(lambda content:f"{result['contexts'][-1]['snapshot']['full_relations']:,} full" in content)
   assert follower.poll() is None
   with stream.open('ab') as out:out.write(data[final[0]:])
   assert follower.wait(timeout=10)==0
  finally:
   if follower.poll() is None:follower.terminate();follower.wait(timeout=10)
 progress=(tmp/'progress.log').read_text()
 assert progress.count('Journal: ')==1 and progress.count('N: ')==1
 assert progress.count('ETA:')>=2 and progress.rstrip().endswith('Collection completed.')
 final_lines=progress.rstrip().split('\n\n')[-1].splitlines()
 assert len(final_lines)==3 and final_lines[1].startswith('recent rates:')
 check('live compact updates print the header once and wait for job completion')
 for label,changed,success in [('failed',rewrite(data,ff[-1],payload=b'\0\0\0\0'+data[ff[-1][0]+info.FRAME.size+4:ff[-1][1]]),True),('bad-status',rewrite(data,ff[-1],payload=b'\x02\0\0\0'+data[ff[-1][0]+info.FRAME.size+4:ff[-1][1]]),False)]:
  altered=tmp/(label+'.journal');altered.write_bytes(changed)
  report=cli(altered,'--follow','--eta','--interval','.01',ok=success)
  if success:assert 'Job ended without a complete factorization.' in report.stdout
 extra=rewrite(data,ff[-1],{2:len(ff)})[ff[-1][0]:]
 after=tmp/'after-end.journal';after.write_bytes(data+extra)
 assert 'frame after final job record' in cli(after,ok=False).stderr
 check('failed jobs finish watching; malformed or nonterminal job markers rejected')
 context_frame=next(v for v in ff if v[2][1]==2)
 for frame,offset in ((context_frame,-8),(batches[0],info.PROGRESS.index('poly_rng')*8)):
  payload=bytearray(data[frame[0]+info.FRAME.size:frame[1]])
  struct.pack_into('<Q',payload,len(payload)+offset if offset<0 else offset,0)
  bad=tmp/'zero-rng.journal';bad.write_bytes(rewrite(data,frame,payload=payload))
  assert 'invalid zero RNG state' in cli(bad,ok=False).stderr
  failed=subprocess.run([str(EXE),'--journal',str(bad)],capture_output=True,text=True,timeout=10)
  assert failed.returncode!=0 and 'invalid zero RNG state' in failed.stderr
 check('zero RNG states rejected in starting and continuation checkpoints')
 for value in (1,101):
  small=tmp/('small-'+str(value)+'.journal')
  subprocess.run([str(EXE),'--journal',str(small),str(value)],stdout=subprocess.DEVNULL,check=True)
  item=inspect(small,True)
  assert item['relation_count']==0 and not item['contexts'] and item['returned_partitions']
 check('zero-relation journals for one and primes validate without contexts')
 restart=tmp/'restart.journal';restart.write_bytes(data[:batches[2][1]])
 subprocess.run([str(EXE),'-t','4','--journal',str(restart)],stdout=subprocess.DEVNULL,check=True)
 resumed=restart.read_bytes();rf=frames(resumed)
 assert [v['id'] for v in inspect(restart)['runs']]==[0,1]
 cutoff=next(v[1] for v in rf if v[2][5]==1 and v[2][1]==3)
 restart.write_bytes(resumed[:cutoff])
 subprocess.run([str(EXE),'-t','1','--journal',str(restart)],stdout=subprocess.DEVNULL,check=True)
 final=restart.read_bytes()
 assert [v['id'] for v in inspect(restart,True)['runs']]==[0,1,2]
 subprocess.run([str(EXE),'--journal',str(restart)],stdout=subprocess.DEVNULL,check=True)
 assert restart.read_bytes()==final
 check('resumed writes increment run; completed replay changes no bytes or run')
 for label,new_run in [('skip',2),('backwards',0)]:
  chosen=[v for v in frames(resumed) if v[2][5]==1][label=='backwards']
  altered=rewrite(resumed,chosen,{5:new_run});bad=tmp/(label+'.journal');bad.write_bytes(altered)
  cli(bad,ok=False)
  p=subprocess.run([str(EXE),'--journal',str(bad)],capture_output=True,text=True)
  assert p.returncode!=0 and 'run counter' in p.stderr and bad.read_bytes()==altered
 check('C/Python reject skipped or decreasing runs without changing file')
 altered=rewrite(data,batches[3],{6:0,7:0});clock=tmp/'clock.journal';clock.write_bytes(altered)
 summary=json.loads(cli(clock,'--json','--verify-relations').stdout)
 assert any('UTC' in v for v in summary['warnings']) and any('elapsed' in v for v in summary['warnings'])
 clock_prefix=tmp/'clock-prefix.journal';clock_prefix.write_bytes(altered[:batches[4][1]])
 assert inspect(clock_prefix)['contexts'][0]['rate'] is None
 subprocess.run([str(EXE),'--journal',str(clock)],stdout=subprocess.DEVNULL,check=True)
 assert clock.read_bytes()==altered
 check('non-monotonic clocks warn; C replay and mathematical checks remain valid')
 partial=tmp/'partial.journal';partial.write_bytes(data[:batches[2][1]+11]);summary=inspect(partial)
 assert summary['tail'] and summary['validated_bytes']==batches[2][1]
 cli(partial,'--strict',ok=False)
 check('incomplete tail preserves prefix; strict mode rejects it')
 changed=bytearray(data);changed[batches[0][0]+info.FRAME.size+8]^=1
 bad=tmp/'crc.journal';bad.write_bytes(changed);cli(bad,ok=False)
 payload=bytearray(data[batches[0][0]+info.FRAME.size:batches[0][1]])
 histogram_at=info.PROGRESS.index('length_1')*8
 struct.pack_into('<Q',payload,histogram_at,struct.unpack_from('<Q',payload,histogram_at)[0]+1)
 bad=tmp/'histogram.journal';bad.write_bytes(rewrite(data,batches[0],payload=payload));cli(bad,ok=False)
 check('bad CRC and inconsistent histogram rejected')
 payload=bytearray(data[batches[0][0]+info.FRAME.size:batches[0][1]])
 width=(result['contexts'][0]['parameters']['large_prime_bound'].bit_length()+7)//8;at=8*len(info.PROGRESS)
 while True:
  kind,labels=struct.unpack_from('BB',payload,at);at+=2
  if kind==2:at+=4*labels
  else:break
 assert kind==1
 at+=labels*width;size=struct.unpack_from('<I',payload,at)[0];assert size>0
 payload[at+4+size-1]^=1
 bad=tmp/'congruence.journal';bad.write_bytes(rewrite(data,batches[0],payload=payload));cli(bad)
 assert 'congruence' in cli(bad,'--verify-relations',ok=False).stderr
 check('optional verification catches checksummed mathematical corruption')
 eta_data=data[:batches[8][1]]
 for frame in frames(eta_data):
  seq=frame[2][2];eta_data=rewrite(eta_data,frame,{6:1700000000000000+seq*1000000,7:seq*1000000})
 eta=tmp/'eta.journal';eta.write_bytes(eta_data);ctx=inspect(eta)['contexts'][0];rate=ctx['rate']
 assert rate and rate['run']==0 and rate['full_per_second']>0
 remaining=max(0,ctx['snapshot']['collection_target']-ctx['snapshot']['full_relations'])
 assert abs(rate['collection_eta_seconds']-remaining/rate['full_per_second'])<1e-9
 fast=inspect(eta,eta_only=True)
 assert fast['relation_count']==inspect(eta)['relation_count']
 assert fast['contexts'][0]['rate']==rate and fast['contexts'][0]['snapshot']==ctx['snapshot']
 check('ETA uses same-run/context monotonic progress')
 solve=tmp/'solve.journal';payload=info.SOLVE.pack(100,103,250,18000000,563000000,12000,77,2,8,1,32,9,1)
 solve.write_bytes(eta_data+metadata_frame(eta_data,7,payload,delay_us=581012000))
 report=inspect(solve,True);baseline=inspect(eta,True)
 assert report['solves'][:-1]==baseline['solves']
 assert report['solves'][-1]==dict(context=1,run=0,method=2,method_name='Lanczos',rows=100,columns=103,
                                 nonzeros=250,matrix_us=18000000,solver_us=563000000,dependency_us=12000,
                                 iterations=77,threads=8,attempts=1,dependencies_returned=32,
                                 dependencies_tested=9,factor_found=True)
 assert report['byte_breakdown']['solve_metadata']==baseline['byte_breakdown'].get('solve_metadata',0)+len(payload)
 assert report['contexts']==baseline['contexts'] and report['relations']==baseline['relations']
 assert report['relation_congruences_verified']==baseline['relation_congruences_verified']
 assert inspect(solve,eta_only=True)['contexts'][0]['rate']==rate
 text=cli(solve).stdout
 assert 'Lanczos, 100 x 103, 250 nonzeros, 8 threads' in text and '9/32 dependencies tested' in text
 attempt_end=next(frame for frame in ff if frame[2][1]==4)
 prefix=data[:attempt_end[0]]
 tail=b''.join(rewrite(data,frame,{2:frame[2][2]+1})[frame[0]:frame[1]]
               for frame in ff[attempt_end[2][2]:])
 solve.write_bytes(prefix+metadata_frame(prefix,7,payload)+tail)
 report=inspect(solve,True)
 assert report['returned_partitions']==result['returned_partitions'] and report['job_end']==result['job_end']
 check('SOLVE fields, solver display and byte accounting preserve relation progress and factors')
 bad=tmp/'bad-solve.journal'
 for prefix,record,count,error in (
     (eta_data,payload[:-1],1,'invalid SOLVE count or payload size'),
     (eta_data,payload,2,'invalid SOLVE count or payload size'),
     (eta_data,info.SOLVE.pack(100,103,250,0,0,0,0,1,1,0,3,4,0),1,'invalid SOLVE dependency counts or result'),
     (data[:ff[0][1]],payload,1,'SOLVE outside active context')):
  bad.write_bytes(prefix+metadata_frame(prefix,7,record,count))
  assert error in cli(bad,'--eta',ok=False).stderr
 check('SOLVE frames reject malformed payloads and placement even in compact ETA mode')
 collection=tmp/'collection.journal';payload=info.COLLECTION.pack(1250000,302500000,12250000,125000)
 collection.write_bytes(eta_data+metadata_frame(eta_data,8,payload,delay_us=316125000))
 report=inspect(collection,True)
 assert report['collections'][:-1]==baseline['collections']
 assert report['collections'][-1]==dict(context=1,run=0,startup_us=1250000,collection_us=302500000,
                                       waiting_us=12250000,drain_us=125000)
 assert report['byte_breakdown']['collection_metadata']==baseline['byte_breakdown'].get('collection_metadata',0)+len(payload)
 assert report['contexts']==baseline['contexts'] and report['relations']==baseline['relations']
 assert report['relation_congruences_verified']==baseline['relation_congruences_verified']
 assert inspect(collection,eta_only=True)['contexts'][0]['rate']==rate
 assert 'startup 1.250s, collection 302.500s, waiting 12.250s, drain 0.125s' in cli(collection).stdout
 prefix=data[:attempt_end[0]]
 collection.write_bytes(prefix+metadata_frame(prefix,8,payload)+tail)
 report=inspect(collection,True)
 assert report['returned_partitions']==result['returned_partitions'] and report['job_end']==result['job_end']
 check('COLLECTION phases, display and byte accounting preserve relation progress and factors')
 bad=tmp/'bad-collection.journal'
 for prefix,record,count,error in (
     (eta_data,payload[:-1],1,'invalid COLLECTION count or payload size'),
     (eta_data,payload,2,'invalid COLLECTION count or payload size'),
     (data[:ff[0][1]],payload,1,'COLLECTION outside active context'),
     (data[:attempt_end[1]],payload,1,'COLLECTION outside active context')):
  bad.write_bytes(prefix+metadata_frame(prefix,8,record,count))
  assert error in cli(bad,'--eta',ok=False).stderr
 check('COLLECTION frames reject malformed payloads and placement even in compact ETA mode')
 accounting=tmp/'accounting.journal'
 cpu=info.ACCOUNTING.pack
 checkpoint=cpu(900000000,0,0,0,40<<20,3,1,2,0)
 final=cpu(3000000000,600000000,36000000,3600000,72<<20,3,3,0,3)
 next_final=cpu(700000000,200000000,24000000,2400000,52<<20,1,1,0,3)
 saved=eta_data+metadata_frame(eta_data,9,checkpoint,delay_us=900000000)
 accounting.write_bytes(saved+metadata_frame(saved,9,next_final,next_run=True))
 partial=inspect(accounting,True)['accounting']
 assert (partial['final_runs'],partial['host_cpu_runs'],partial['runs'],partial['journal_runs'])==(1,1,2,2)
 assert partial['partial'] and partial['checkpointed_jobs']==2
 saved+=metadata_frame(saved,9,final)
 accounting.write_bytes(saved+metadata_frame(saved,9,next_final,next_run=True))
 report=inspect(accounting,True)
 measured=report['accounting']
 assert measured==dict(worker_user_us=3700000000,worker_system_us=800000000,
                       host_user_us=60000000,host_system_us=6000000,peak_rss_bytes=72<<20,
                       jobs=4,timed_jobs=4,checkpointed_jobs=0,runs=2,final_runs=2,host_cpu_runs=2,
                       journal_runs=2,partial=False)
 assert json.loads(cli(accounting,'--json').stdout)['accounting']==measured
 assert report['byte_breakdown']['accounting_metadata']==3*info.ACCOUNTING.size
 assert report['contexts']==baseline['contexts'] and report['relations']==baseline['relations']
 assert report['relation_congruences_verified']==baseline['relation_congruences_verified']
 assert inspect(accounting,eta_only=True)['contexts'][0]['rate']==rate
 text=cli(accounting).stdout
 assert 'CPU accounting (complete) 1.250000 worker core-hours' in text and 'peak host RSS 72.0 MiB' in text
 assert baseline['accounting'] is None
 check('ACCOUNTING uses the latest snapshot per run and preserves relation progress')
 accounting.write_bytes(eta_data+metadata_frame(eta_data,9,next_final,next_run=True))
 missing=inspect(accounting)['accounting']
 assert missing['runs']==missing['final_runs']==missing['host_cpu_runs']==1 and missing['journal_runs']==2
 assert missing['partial']
 for record in (cpu(1,0,0,0,0,1,1,0,2), cpu(1,0,1,0,0,2,1,1,3)):
  accounting.write_bytes(eta_data+metadata_frame(eta_data,9,record))
  assert inspect(accounting)['accounting']['partial']
  assert 'CPU accounting (partial)' in cli(accounting).stdout
 last=ff[-1];prefix=data[:last[0]]
 tail=rewrite(data,last,{2:last[2][2]+1})[last[0]:]
 accounting.write_bytes(prefix+metadata_frame(prefix,9,final)+tail)
 report=inspect(accounting,True)
 assert report['returned_partitions']==result['returned_partitions'] and report['job_end']==result['job_end']
 check('ACCOUNTING shows incomplete invocation or CPU coverage and preserves final factors')
 bad=tmp/'bad-accounting.journal'
 for prefix,record,count,next_run,error in (
     (eta_data,checkpoint[:-1],1,False,'invalid ACCOUNTING count or payload size'),
     (eta_data,checkpoint,2,False,'invalid ACCOUNTING count or payload size'),
     (eta_data,cpu(0,0,0,0,0,0,0,0,4),1,False,'invalid ACCOUNTING flags'),
     (eta_data,cpu(0,0,1,0,0,0,0,0,0),1,False,'host times without host CPU flag'),
     (eta_data,cpu(0,0,0,0,0,2,2,1,0),1,False,'invalid ACCOUNTING job counts'),
     (data[:ff[0][1]],checkpoint,1,False,'ACCOUNTING checkpoint outside BATCH boundary'),
     (eta_data,checkpoint,1,True,'ACCOUNTING checkpoint outside BATCH boundary'),
     (data,final,1,False,'frame after final job record')):
  bad.write_bytes(prefix+metadata_frame(prefix,9,record,count,next_run=next_run))
  assert error in cli(bad,'--eta',ok=False).stderr
 check('ACCOUNTING rejects malformed payloads and checkpoint placement even in compact ETA mode')
 live=tmp/'live.journal'
 with (tmp/'collector.log').open('w') as collector_log,(tmp/'follow.log').open('w') as follow_log:
  collector=subprocess.Popen([str(EXE),'-t','1','--journal',str(live),str(TWO)],stdout=collector_log,stderr=collector_log)
  follower=None
  try:
   deadline=time.monotonic()+30
   while time.monotonic()<deadline:
    try:
     if inspect(live)['frames']>=3:break
    except (OSError,info.InvalidJournal,ValueError):pass
    assert collector.poll() is None;time.sleep(.02)
   else:raise AssertionError('no live prefix')
   follower=subprocess.Popen([sys.executable,str(TOOL),'--follow','--json','--eta','--interval','.02',str(live)],stdout=follow_log,stderr=follow_log)
   while time.monotonic()<deadline:
    lines=(tmp/'follow.log').read_text().splitlines()
    if len(lines)>=2:
     reports=[json.loads(v) for v in lines]
     assert reports[-1]['frames']>reports[0]['frames'] and collector.poll() is None
     break
    assert follower.poll() is None;time.sleep(.02)
   else:raise AssertionError('no incremental output')
  finally:
   if collector.poll() is None:collector.terminate()
   collector.wait(timeout=10)
   if follower is not None and follower.poll() is None:
    follower.send_signal(signal.SIGINT);assert follower.wait(timeout=10)==0
 check('live follow reads actively locked writer and reports incremental progress')
print(f'All {passed}/{passed} journal-info checks passed.',flush=True)
