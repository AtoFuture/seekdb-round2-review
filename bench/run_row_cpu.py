"""Alternating same-binary row CPU comparisons; not official scoring."""
from pathlib import Path
import argparse, hashlib, json, statistics, subprocess, tempfile

p=argparse.ArgumentParser()
p.add_argument('--output',required=True)
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--suite',choices=['dense','varied','profile'],default='dense')
p.add_argument('--case')
p.add_argument('--modes',default='reference,batched')
a=p.parse_args()
root=Path('/mnt/d/Codex/MiniVectorDB')
out=Path(a.output).resolve()
assert out.is_relative_to(root) and not out.exists()
binary=root/'build/row-cpu-bench'
source_hash=hashlib.sha256((root/'repo/bench/paired.cpp').read_bytes()).hexdigest()
image='sha256:463556d0233119a7bec1956179ddba76ce61825c135308e5bebd1d4dbb5ec563'
docker=['docker','--config','/var/lib/minivectordb-docker-client']
data_root=Path('/var/lib/minivectordb-paired-bench');data_root.mkdir(exist_ok=True)
modes=a.modes.split(',')
assert len(modes)>=2 and len(set(modes))==len(modes)
settings={
 'reference':{'MVDB_VECTOR_SCALAR':'1','MVDB_ROW_HEAP':'1'},
 'batched':{'MVDB_ROW_HEAP':'1'},
 'inline':{},
 'uploaded':{'MVDB_VECTOR_SCALAR':'1','MVDB_ROW_HEAP':'1'},
}
assert set(modes)<=set(settings)
binary_paths={mode:(root/'build/row-cpu-uploaded-bench' if mode=='uploaded' else binary) for mode in modes}
binary_hashes={mode:hashlib.sha256(path.read_bytes()).hexdigest() for mode,path in binary_paths.items()}
cases={'dense':(10000,200,{})}
if a.suite=='varied':
 cases={'unicode':(2000,40,{'MVDB_UNICODE_BODY':'1'}),
        'wide':(2000,40,{'MVDB_DIMENSIONS':'768'}),
        'overflow':(2000,40,{'MVDB_PAYLOAD':'5000'}),
        'interleaved':(2000,40,{'MVDB_INVALIDATE_EVERY':'1'}),
        'small':(2000,40,{'MVDB_DIMENSIONS':'16'}),
        'sparse':(2000,40,{'MVDB_SPARSE_KEYS':'1'}),
        'churn':(2000,40,{'MVDB_CHURN':'1'}),
        'no_snapshot':(2000,40,{'MVDB_SNAPSHOT_BYTES':'0'}),
        'eight_columns':(2000,40,{'MVDB_EXTRA_COLUMNS':'4'}),
        'nine_columns':(2000,40,{'MVDB_EXTRA_COLUMNS':'5'}),
        'wide_schema':(2000,40,{'MVDB_EXTRA_COLUMNS':'60'})}
if a.suite=='profile':cases={'dense':(2000,20,{}),'wide':(2000,40,{'MVDB_DIMENSIONS':'768'})}
if a.case:cases={a.case:cases[a.case]}
records=[]
for case,(rows,queries,extra) in cases.items():
 for round_no in range(a.rounds):
  order=modes[round_no%len(modes):]+modes[:round_no%len(modes)]
  if (round_no//len(modes))%2:order.reverse()
  for mode in order:
   run_binary=binary_paths[mode]
   print('START',case,round_no+1,mode,flush=True)
   with tempfile.TemporaryDirectory(prefix='owned-',dir=data_root) as directory:
    env={'MVDB_DATA_CACHE_BYTES':'16777216','MVDB_SCAN_CACHE_BYTES':'0',
         'MVDB_PACKED':'1','MVDB_BENCH_CGROUP':'1','MVDB_PAIRED':'1',
         # Isolate historical row CPU factors from later buffer optimizations.
         # Old frozen binaries ignore these flags and already use this behavior.
         'MVDB_VECTOR_SCRATCH':'0','MVDB_REUSE_SNAPSHOT':'0',
         'MVDB_SNAPSHOT_BYTES':'8388608',**extra,**settings[mode]}
    if a.suite=='profile':env['MVDB_PROFILE']='1'
    cmd=docker+['create','--read-only','--network','none','--cpus','2','--memory','512m',
                '--mount',f'type=bind,src={directory},dst=/data',
                '--mount',f'type=bind,src={run_binary},dst=/bench/run,readonly','--entrypoint','/bench/run']
    for key,value in env.items():cmd+=['--env',key+'='+value]
    cmd += [image,'/data/database.mvdb',str(rows),str(queries),'1','1']
    container=subprocess.check_output(cmd,text=True,timeout=60).strip()
    try:
     run=subprocess.run(docker+['start','--attach',container],text=True,capture_output=True,timeout=900)
     if run.returncode:raise RuntimeError(run.stdout+run.stderr)
     row=json.loads(run.stdout)
    finally:subprocess.run(docker+['rm','--force',container],check=True,capture_output=True,timeout=30)
    assert row['vector_scalar']==('MVDB_VECTOR_SCALAR' in settings[mode])
    assert row['row_heap']==('MVDB_ROW_HEAP' in settings[mode])
    assert not row.get('reuse_vector_scratch', False)
    assert not row.get('reuse_scan_snapshot_buffer', False)
    assert row['paired_index_scan'] and row['snapshot_budget']==int(env['MVDB_SNAPSHOT_BYTES'])
    assert row['validated_queries']==queries and row['first_query_validated']
    assert row['recovered_rows']==(rows*3//4 if case=='churn' else rows)
    row.update(case=case,mode=mode,round=round_no+1,binary_sha256=binary_hashes[mode],
               benchmark_source_sha256=source_hash,base_commit='cc0ae3f5418e8d56ebdb9530e83c96a85c8a36ba',
               scope=('uploaded-core/final-core comparison' if 'uploaded' in modes else 'same-binary ablation')+'; workload/results memory included through query phase, not official B/M/E')
    records.append(row)
    with out.open('a') as f:f.write(json.dumps(row)+'\n')
    print('PASS query_ms',row['query_total_ms'],'bytes',row['database_bytes'],flush=True)
summary={}
for case in cases:
 group=[r for r in records if r['case']==case]
 for key in ['result_digest','database_bytes','rows_examined','distances_computed','recovered_rows','cells_materialized','vector_values_materialized']:
  assert len({r[key] for r in group})==1,(case,key)
 summary[case]={}
 for mode in modes:
  group_mode=[r for r in group if r['mode']==mode]
  summary[case][mode]={key:statistics.median(r[key] for r in group_mode) for key in [
   'write_ms','index_build_ms','query_total_ms','query_unfiltered_ms','query_filtered_ms',
   'first_query_ms','snapshot_capacity','snapshot_rows','database_bytes','churn_ms',
   'process_maxrss_kib','local_cgroup_peak_bytes']}
for mode,path in binary_paths.items():assert hashlib.sha256(path.read_bytes()).hexdigest()==binary_hashes[mode]
assert hashlib.sha256((root/'repo/bench/paired.cpp').read_bytes()).hexdigest()==source_hash
report={'medians':summary,'samples':len(records),'validated_queries':sum(r['validated_queries'] for r in records),
        'reopen_checks':len(records),'diagnostic':a.suite=='profile','no_official_score':True}
out.with_suffix('.summary.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report),flush=True)
