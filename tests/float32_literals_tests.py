import json,os,pathlib,subprocess,sys,tempfile
root=pathlib.Path(os.environ.get('MVDB_TEST_DIR',tempfile.gettempdir()));root.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=root) as d:
 def run(sqls):
  requests=[dict(id=i,op='execute',sql=s,search_mode='ann' if i%2 else 'exact') for i,s in enumerate(sqls)]+[dict(id=99,op='shutdown')]
  p=subprocess.run([sys.argv[1],'--protocol','jsonl','--data-dir',d],input=''.join(json.dumps(r)+'\n' for r in requests),capture_output=True,text=True,timeout=60)
  assert p.returncode==0,p.stderr
  out=[json.loads(s) for s in p.stdout.splitlines()];assert out.pop(0)=={'type':'ready'}
  assert len(out)==len(requests) and all(r['id']==q['id'] for r,q in zip(out,requests));assert out[-1]['ok'];return out[:-1]
 values=['1e-46','1e-320','1e-324','1e-400','-1e-400','1e-9999','0e9999']
 r=run(['CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1));']+[f"INSERT INTO t VALUES({i},'[{x}]');" for i,x in enumerate(values)]+['CREATE VECTOR INDEX ix ON t(v);',"SELECT id,L2_DISTANCE(v,'[-1e-9999]') AS d FROM t ORDER BY d LIMIT 10;",'BEGIN;',"INSERT INTO t VALUES(50,'[2]');","INSERT INTO t VALUES(51,'[1e-9999]'),(52,'[1e9999]');",'COMMIT;'])
 assert all(x['ok'] for x in r[:-2]),r
 assert not r[-2]['ok'] and r[-1]['ok'],r
 assert r[9]['rows']==[[i,0] for i in range(7)],r
 r=run(['SELECT id,v FROM t ORDER BY id;',"SELECT id,L2_DISTANCE(v,'[1e-400]') AS d FROM t ORDER BY d LIMIT 3;"])
 assert r[0]['rows']==[[i,[0]] for i in range(7)]+[[50,[2]]],r
 assert r[1]['rows']==[[0,0],[1,0],[2,0]],r
print('PASS extreme literals: signed underflow, true overflow rejection, index, transaction rollback, reopen')
