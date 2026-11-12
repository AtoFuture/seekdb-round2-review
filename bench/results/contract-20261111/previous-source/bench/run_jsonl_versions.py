"""Counterbalanced real-runtime comparison against a configurable baseline image."""
from pathlib import Path
import argparse,json,os,statistics,subprocess,sys

p=argparse.ArgumentParser()
p.add_argument('--current-image',required=True)
p.add_argument('--reference-image',default='sha256:463556d0233119a7bec1956179ddba76ce61825c135308e5bebd1d4dbb5ec563')
p.add_argument('--reference-label',default='cc0ae3f5418e8d56ebdb9530e83c96a85c8a36ba')
p.add_argument('--current-label',default='cc0ae3f5418e8d56ebdb9530e83c96a85c8a36ba + row CPU working tree')
p.add_argument('--output-prefix',required=True)
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--oracle-bin',default='/mnt/d/Codex/MiniVectorDB/work/jsonl-snapshot-oracle')
a=p.parse_args()
root=Path('/mnt/d/Codex/MiniVectorDB')
assert a.rounds>0 and '/' not in a.output_prefix and '\\' not in a.output_prefix
images={'uploaded':a.reference_image,
        'current':a.current_image}
samples=[];metadata=[]
out=root/'work'/(a.output_prefix+'-summary.json')
assert not out.exists()
for round_no in range(1,a.rounds+1):
 for version in (['uploaded','current'] if round_no%2 else ['current','uploaded']):
  name=a.output_prefix+'-'+version+'-r'+str(round_no)
  env=dict(os.environ,MVDB_BENCH_IMAGE=images[version],
           MVDB_BENCH_COMMIT=a.current_label if version=='current' else a.reference_label,
           MVDB_ORACLE_BIN=a.oracle_bin,MVDB_BENCH_INDEX_FIRST=str((round_no-1)%2))
  print('START',name,flush=True)
  log=root/'work'/(name+'.log')
  assert not log.exists()
  with log.open('w') as f:
   subprocess.run([sys.executable,str(root/'repo/bench/run_jsonl_baseline.py'),'10000','1000','1',name],
                  env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
  result=json.loads((root/'work'/name/'summary.json').read_text())
  metadata.append(result['metadata'])
  for sample in result['samples']:
   assert sample['passed'] and sample['correct_queries']==1000 and sample['restart_check']
   sample.update(version=version,comparison_round=round_no,source_directory=name)
   samples.append(sample)
  print('PASS',name,'indexed_ms',result['indexed']['median_query_ms'],
        'unindexed_ms',result['unindexed']['median_query_ms'],flush=True)
assert len({m['oracle_input_sha256'] for m in metadata})==1
summary={}
for version in images:
 summary[version]={}
 for indexed in [False,True]:
  group=[s for s in samples if s['version']==version and s['indexed']==indexed]
  metrics={key:statistics.median(s[key] for s in group) for key in [
   'write_ms','query_ms','unfiltered_query_ms','filtered_query_ms']}
  metrics['index_build_ms']=statistics.median(s['index_build_ms'] for s in group) if indexed else None
  metrics['peak_bytes']=statistics.median(int(s['resources_after_queries']['memory.peak']) for s in group)
  metrics['database_bytes']=statistics.median(sum(s['files_final'].values()) for s in group)
  summary[version]['indexed' if indexed else 'unindexed']=metrics
report={'medians':summary,'samples':samples,'images':images,'metadata':metadata,
        'queries_verified':sum(s['correct_queries'] for s in samples),'restarts':len(samples),
        'official_score':None,'order':'version order and indexed/unindexed order both alternate by round'}
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(summary),flush=True)
