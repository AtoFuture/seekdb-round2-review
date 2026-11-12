"""Interleaved same-binary indexed scan comparison; local diagnostic, not official score."""
from pathlib import Path
import argparse,hashlib,json,statistics,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--rounds',type=int,default=3)
p.add_argument('--suite',choices=['dense','varied','profile'],default='dense');p.add_argument('--case',default=None);a=p.parse_args()
root=Path('/mnt/d/Codex/MiniVectorDB');out=Path(a.output).resolve()
assert out.is_relative_to(root) and not out.exists()
binary=root/'build/paired-bench';binary_hash=hashlib.sha256(binary.read_bytes()).hexdigest()
source_hash=hashlib.sha256((root/'repo/bench/paired.cpp').read_bytes()).hexdigest()
image='sha256:df51f7631fcf2e9ff44f93234eb64d0ed5132441e490e50cf5b7e8e528e87321'
docker=['docker','--config','/var/lib/minivectordb-docker-client']
data_root=Path('/var/lib/minivectordb-paired-bench');data_root.mkdir(exist_ok=True)
cases={'dense':(10000,200,{})}
if a.suite=='varied':cases={'sparse':(2000,40,{'MVDB_SPARSE_KEYS':'1'}),'wide':(2000,40,{'MVDB_DIMENSIONS':'768'}),'overflow':(2000,40,{'MVDB_PAYLOAD':'5000'}),'overcache':(30000,40,{}),'churn':(2000,40,{'MVDB_CHURN':'1'})}
if a.suite=='profile':cases={'dense':(2000,20,{}),'sparse':(2000,20,{'MVDB_SPARSE_KEYS':'1'})}
if a.case:cases={a.case:cases[a.case]}
records=[]
for case,(rows,queries,extra) in cases.items():
 for round_no in range(a.rounds):
  for paired in ([False,True] if round_no%2==0 else [True,False]):
   print('START',case,round_no+1,'paired',paired,flush=True)
   with tempfile.TemporaryDirectory(prefix='owned-',dir=data_root) as directory:
    env={'MVDB_DATA_CACHE_BYTES':'16777216','MVDB_SCAN_CACHE_BYTES':'0','MVDB_PACKED':'1','MVDB_BENCH_CGROUP':'1',**extra}
    if paired:env['MVDB_PAIRED']='1'
    if a.suite=='profile':env['MVDB_PROFILE']='1'
    cmd=docker+['create','--read-only','--network','none','--cpus','2','--memory','512m','--mount',f'type=bind,src={directory},dst=/data','--mount',f'type=bind,src={binary},dst=/bench/run,readonly','--entrypoint','/bench/run']
    for key,value in env.items():cmd+=['--env',key+'='+value]
    cmd += [image,'/data/database.mvdb',str(rows),str(queries),'1','1']
    container=subprocess.check_output(cmd,text=True,timeout=60).strip()
    try:
     run=subprocess.run(docker+['start','--attach',container],text=True,capture_output=True,timeout=900)
     if run.returncode:raise RuntimeError(run.stdout+run.stderr)
     row=json.loads(run.stdout)
    finally:subprocess.run(docker+['rm','--force',container],check=True,capture_output=True,timeout=30)
    assert row['paired_index_scan']==paired and row['validated_queries']==queries and row['first_query_validated']
    assert row['recovered_rows']==(rows*3//4 if case=='churn' else rows)
    row.update(case=case,round=round_no+1,binary_sha256=binary_hash,benchmark_source_sha256=source_hash,base_commit='0e954ffd90c6802ecd9fae4c0ca00eb8553201dd',scope='same-binary local core comparison; harness/truth memory included, not official B/M/E')
    records.append(row)
    with out.open('a') as f:f.write(json.dumps(row)+'\n')
    print('PASS query_ms',row['query_total_ms'],'bytes',row['database_bytes'],flush=True)
summary={}
for case in cases:
 group=[r for r in records if r['case']==case]
 for key in ['result_digest','database_bytes','rows_examined','distances_computed','vector_values_materialized','recovered_rows']:
  assert len({r[key] for r in group})==1,(case,key)
 summary[case]={}
 for paired in [False,True]:
  rows=[r for r in group if r['paired_index_scan']==paired]
  summary[case][str(paired)]={key:statistics.median(r[key] for r in rows) for key in ['write_ms','index_build_ms','query_total_ms','query_unfiltered_ms','query_filtered_ms','first_query_ms','database_bytes','churn_ms','process_maxrss_kib','local_cgroup_peak_bytes']}
assert hashlib.sha256(binary.read_bytes()).hexdigest()==binary_hash
assert hashlib.sha256((root/'repo/bench/paired.cpp').read_bytes()).hexdigest()==source_hash
report={'medians':summary,'samples':len(records),'validated_queries':sum(r['validated_queries'] for r in records),'reopen_checks':len(records),'diagnostic':a.suite=='profile','no_official_score':True}
out.with_suffix('.summary.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report),flush=True)
