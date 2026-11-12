"""Host-generated data larger than the database container's memory budget."""
import json,math,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
sys.path.insert(0,str(root/'official-protocol-f4d4e26/protocol_check'))
from _transport import Session
cfg=json.loads((root/'official-protocol-f4d4e26/protocol_check/config.json').read_text())
cfg.update(memory='64m',case_timeout=1800,request_timeout=120)
out=root/'snapshot-resource';out.mkdir(exist_ok=False)
data=Path('/var/lib/minivectordb-snapshot-resource');data.mkdir(exist_ok=True)
s=Session(cfg,out/'stderr.log',image=sys.argv[1],data_root=str(data))
n=20000;body='x'*4096
def q(sql):
 r=s.request(sql);assert r['ok'],r;return r
def stats():
 inspection=json.loads(s.docker('inspect',s.name).stdout)[0]
 pid=inspection['State']['Pid']
 cg=next(t.split(':',2)[2] for t in Path('/proc/%d/cgroup'%pid).read_text().splitlines() if t.startswith('0::'))
 base=Path('/sys/fs/cgroup')/cg.lstrip('/')
 return {'memory_limit':inspection['HostConfig']['Memory'],'memory_peak':int((base/'memory.peak').read_text()),'swap_current':int((base/'memory.swap.current').read_text()),'oom_killed':inspection['State']['OOMKilled'],'file_bytes':sum(p.stat().st_size for p in Path(s.bind_data.name).rglob('*') if p.is_file())}
try:
 s.start();q('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),body VARCHAR(4096))')
 for begin in range(0,n,100):
  sql='INSERT INTO t VALUES '+','.join("(%d,'[%d,%d]','%s')"%(i,i%97,i%13,body) for i in range(begin,begin+100))
  assert q(sql)['affected_rows']==100
 q('CREATE VECTOR INDEX tv ON t(v)')
 expected=[[i,d] for d,i in sorted((math.sqrt((i%97)**2+(i%13)**2),i) for i in range(n))[:10]]
 for attempt in range(4):assert q("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10")['rows']==expected
 assert len(q('SELECT id FROM t')['rows'])==n
 before=stats();assert before['memory_limit']==64*1024*1024 and before['file_bytes']>before['memory_limit'] and not before['oom_killed']
 q('BEGIN');q('DELETE FROM t WHERE id=0');s.crash();s.start()
 assert q('SELECT body FROM t WHERE id=0')['rows']==[[body]]
 assert q("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10")['rows']==expected
 after=stats();s.shutdown()
 result={'passed':True,'rows':n,'logical_text_bytes':n*len(body),'before_restart':before,'after_restart':after,'scope':'64MiB container limit; observed swap reported, host WSL cannot enforce no-swap; not official resource admission'}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:s.close()
