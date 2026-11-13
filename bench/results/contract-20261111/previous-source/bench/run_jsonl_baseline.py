"""Exact-runtime Docker JSONL baseline; independent host oracle, no engine linkage."""
import sys,os,json,time,random,struct,math,subprocess,hashlib,statistics
from pathlib import Path
ROOT=Path('/mnt/d/Codex/MiniVectorDB')
sys.path.insert(0,str(ROOT/'work/official-protocol-f4d4e26/protocol_check'))
from _transport import Session
IMAGE=os.environ.get('MVDB_BENCH_IMAGE','sha256:df51f7631fcf2e9ff44f93234eb64d0ed5132441e490e50cf5b7e8e528e87321')
assert sys.flags.optimize==0, 'Assertions require normal Python mode'
N,Q,R=map(int,sys.argv[1:4])
index_order_offset=int(os.environ.get('MVDB_BENCH_INDEX_FIRST','0'))
assert index_order_offset in (0,1)
OUT=ROOT/'work'/sys.argv[4];OUT.mkdir(exist_ok=False)
config=json.loads((ROOT/'work/official-protocol-f4d4e26/protocol_check/config.json').read_text())
config.update(request_timeout=120,case_timeout=14400)
rng=random.Random(20261110)
vectors=[[rng.randrange(-2048,2049)/128 for _ in range(128)] for i in range(N)]
queries=[[rng.randrange(-2048,2049)/128 for _ in range(128)] for i in range(Q)]
order=list(range(N));rng.shuffle(order)
body=lambda i:'record-'+str(i)+'-'+chr(97+i%26)*256
literal=lambda v:json.dumps(v,separators=(',',':'))
inserts=['INSERT INTO t VALUES '+','.join("(%d,%d,'%s','%s')"%(i,i%10,literal(vectors[i]),body(i)) for i in order[a:a+100])+';' for a in range(0,N,100)]
sqls=["SELECT id,L2_DISTANCE(v,'%s') AS d,body FROM t%s ORDER BY d LIMIT 10;"%(literal(v),(' WHERE g='+str((i//2)%10)) if i%2 else '') for i,v in enumerate(queries)]
inputpath=OUT/'oracle-input.bin'
with inputpath.open('wb') as f:
 f.write(struct.pack('<III',N,Q,128))
 for v in vectors+queries:f.write(struct.pack('<128f',*v))
truthpath=OUT/'oracle-truth.txt'
oracle=Path(os.environ.get('MVDB_ORACLE_BIN',str(ROOT/'work/jsonl-baseline-oracle')))
with truthpath.open('w') as f:subprocess.run([str(oracle),str(inputpath)],stdout=f,check=True)
truth=[[(int(p.split(':')[0]),float(p.split(':')[1])) for p in line.split()] for line in truthpath.read_text().splitlines()]
assert len(truth)==Q
meta={'commit':os.environ.get('MVDB_BENCH_COMMIT','0e954ffd90c6802ecd9fae4c0ca00eb8553201dd'),'runtime':IMAGE,'rows':N,'dimensions':128,'queries_per_round':Q,'rounds':R,'batch':100,'seed':20261110,'payload_chars':256,'top_k':10,'half_filtered_group_selectivity':0.1,'search_mode':'exact','index_creation':'after insertion','warmup_queries':10,'timing':'Session.request includes JSON serialization, pipe/Docker transport, server execution, response parsing; excludes dataset/oracle generation and answer verification','oracle_input_sha256':hashlib.sha256(inputpath.read_bytes()).hexdigest(),'constraints':config,'swap_caveat':'WSL SwapLimit=false; requested --memory-swap=2g not enforceable; global settings unchanged','official_score':None}
meta.update(index_order_offset=index_order_offset,oracle_binary_sha256=hashlib.sha256(oracle.read_bytes()).hexdigest())
(OUT/'metadata.json').write_text(json.dumps(meta,indent=2))
def resource(s):
 result={}
 try:
  pid=int(s.docker('inspect','--format','{{.State.Pid}}',s.name).stdout)
  cgroup=next(x.split(':',2)[2] for x in Path('/proc/%d/cgroup'%pid).read_text().splitlines() if x.startswith('0::'))
  base=Path('/sys/fs/cgroup')/cgroup.lstrip('/')
  device=os.stat(s.bind_data.name).st_dev
  result['data_device']=str(os.major(device))+':'+str(os.minor(device))
  result['cgroup_path']=cgroup
  for name in ['memory.peak','memory.current','memory.swap.current','memory.stat','memory.events','io.stat','cpu.stat']:
   try: result[name]=(base/name).read_text().strip()
   except OSError as e:result[name]=None;result[name+'_reason']=str(e)
 except Exception as e:result={'unavailable_reason':str(e)}
 return result
def verify(response,j):
 assert response.get('ok') is True,response
 assert response['columns']==['id','d','body'],response
 assert len(response['rows'])==len(truth[j])
 for actual,(ident,dist) in zip(response['rows'],truth[j]):
  assert actual[0]==ident and actual[2]==body(ident) and math.isclose(actual[1],dist,rel_tol=1e-10,abs_tol=1e-10),(j,actual,ident,dist)
def execute(s,sql):
 start=time.perf_counter_ns();response=s.request(sql,mode='exact');ms=(time.perf_counter_ns()-start)/1e6
 assert response.get('ok') is True,response
 return response,ms
def files(s):
 return {str(p.relative_to(s.bind_data.name)):p.stat().st_size for p in Path(s.bind_data.name).rglob('*') if p.is_file()}
results=[]
for roundno in range(R):
 for indexed in ([False,True] if (roundno+index_order_offset)%2==0 else [True,False]):
  tag='round%d-%s'%(roundno+1,'indexed' if indexed else 'unindexed')
  s=Session(config,OUT/(tag+'-stderr.log'),image=IMAGE,case_timeout=14400,data_root='/var/lib/minivectordb-jsonl-bench')
  result={'round':roundno+1,'indexed':indexed,'tag':tag}
  try:
   s.start();result['resources_ready']=resource(s)
   _,result['create_table_ms']=execute(s,'CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(128),body VARCHAR(512));')
   writes=[]
   for sql in inserts: _,ms=execute(s,sql);writes.append(ms)
   result['write_ms']=sum(writes);result['write_request_ms']=writes;result['files_after_write']=files(s)
   result['resources_after_write']=resource(s)
   result['index_build_ms']=None
   if indexed:_,result['index_build_ms']=execute(s,'CREATE VECTOR INDEX tv ON t(v);')
   result['files_after_index']=files(s);result['resources_after_load']=resource(s)
   warms=[]
   for j in range(10):
    response,ms=execute(s,sqls[j%Q]);verify(response,j%Q);warms.append(ms)
   result['warmup_request_ms']=warms;result['resources_before_queries']=resource(s)
   latencies=[]
   for j,sql in enumerate(sqls):
    response,ms=execute(s,sql);latencies.append(ms);verify(response,j)
    if (j+1)%100==0:print(json.dumps({'progress':tag,'queries':j+1,'elapsed_query_ms':sum(latencies)}),flush=True)
   result['query_request_ms']=latencies;result['query_ms']=sum(latencies)
   result['unfiltered_query_ms']=sum(latencies[::2]);result['filtered_query_ms']=sum(latencies[1::2])
   result['resources_after_queries']=resource(s);result['files_final']=files(s)
   for j in range(min(4,Q)):verify(s.request(sqls[j],mode='ann'),j)
   result['ann_extra_checks']=min(4,Q);result['correct_queries']=Q
   s.shutdown();s.start()
   verify(s.request(sqls[0]),0);result['restart_check']=True;s.shutdown()
   result['passed']=True
   results.append(result);(OUT/(tag+'.json')).write_text(json.dumps(result,indent=2))
   print(json.dumps({k:result[k] for k in ['tag','write_ms','index_build_ms','query_ms','unfiltered_query_ms','filtered_query_ms','passed']}),flush=True)
  except BaseException as e:
   result.update(passed=False,error=repr(e));(OUT/(tag+'-failed.json')).write_text(json.dumps(result,indent=2));raise
  finally:s.close()
summary={'metadata':meta,'samples':[{k:v for k,v in r.items() if k not in ['query_request_ms','write_request_ms']} for r in results]}
for indexed in [False,True]:
 rs=[r for r in results if r['indexed']==indexed]
 summary['indexed' if indexed else 'unindexed']={'median_write_ms':statistics.median(r['write_ms'] for r in rs),'median_query_ms':statistics.median(r['query_ms'] for r in rs),'median_index_build_ms':statistics.median(r['index_build_ms'] for r in rs) if indexed else None}
(OUT/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps({k:summary[k] for k in ['indexed','unindexed']}),flush=True)
