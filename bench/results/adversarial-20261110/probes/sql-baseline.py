import json, pathlib, subprocess, select, random, math, functools, tempfile, os, traceback
ROOT=pathlib.Path('/mnt/d/Codex/MiniVectorDB')
BIN=ROOT/'build/row-cpu-core/minivectordb'
R=random.Random(548219)
checks=0
transcript=[]

def quote(s): return "'"+s.replace("'","''")+"'"
def vector(v): return '['+','.join(map(str,v))+']'
def row_sql(k,r): return '('+str(r[0])+','+vector(r[1])+','+quote(r[2])+','+str(k)+')'
def dist(v,q): return math.sqrt(sum((float(a)-float(b))**2 for a,b in zip(v,q)))
class DB:
 def __init__(self, path):
  self.path=path; self.p=None; self.start()
 def start(self):
  self.p=subprocess.Popen([str(BIN),'--data-dir',str(self.path),'--sort-memory','4096'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  assert self.read()=={'type':'ready'}
 def read(self):
  assert select.select([self.p.stdout],[],[],12)[0], 'timeout'
  line=self.p.stdout.readline()
  assert line, 'EOF '+self.p.stderr.read().decode()
  return json.loads(line)
 def q(self,sql,ok=True,mode='exact'):
  global checks
  transcript.append((sql,ok,mode)); checks+=1
  self.p.stdin.write((json.dumps({'id':checks,'op':'execute','sql':sql,'search_mode':mode})+'\n').encode()); self.p.stdin.flush()
  res=self.read(); assert res['ok']==ok, (sql,res,ok)
  return res
 def close(self):
  self.p.stdin.write(b'{"id":0,"op":"shutdown"}\n'); self.p.stdin.flush(); assert self.read()['ok']; assert self.p.wait(timeout=12)==0
  for f in (self.p.stdin,self.p.stdout,self.p.stderr):f.close()
 def restart(self): self.close();self.start()

def query(db,name,rows,kind='near',lim=None,pred=None,mode='exact',q=(0,0,0),q2=(2,-1,4)):
 where='' if pred is None else ' WHERE '+pred[0]
 selected=[(k,r) for k,r in rows.items() if pred is None or pred[1](k,r)]
 def record(k,r):return [k,r[0],r[2],list(r[1]),dist(r[1],q),dist(r[1],q2)]
 if kind=='near': order='d'; key=lambda a:(dist(a[1][1],q),a[0])
 elif kind=='far': order='d DESC'; key=lambda a:(-dist(a[1][1],q),a[0])
 elif kind=='multi': order='d ASC,e DESC,n DESC,id DESC'; key=lambda a:(dist(a[1][1],q),-dist(a[1][1],q2),-a[1][0],-a[0])
 elif kind=='text':
  order='s DESC,n ASC'
  def cmp(a,b):
   for av,bv,desc in [(a[1][2],b[1][2],True),(a[1][0],b[1][0],False),(a[0],b[0],False)]:
    c=(av>bv)-(av<bv)
    if c:return -c if desc else c
   return 0
  key=functools.cmp_to_key(cmp)
 else: order='n DESC,id ASC';key=lambda a:(-a[1][0],a[0])
 sql='SELECT id,n,s,v,L2_DISTANCE(v,'+vector(q)+') AS d,L2_DISTANCE(v,'+vector(q2)+') AS e FROM '+name+where+' ORDER BY '+order+((' LIMIT '+str(lim)) if lim is not None else '')
 actual=db.q(sql,mode=mode)['rows']; selected.sort(key=key)
 expected=[record(k,r) for k,r in selected[:lim]] if lim is not None else [record(k,r) for k,r in selected]
 assert len(actual)==len(expected),(sql,len(actual),len(expected))
 for a,e in zip(actual,expected):
  assert a[:4]==e[:4] and all(math.isclose(av,ev,rel_tol=1e-12,abs_tol=1e-12) for av,ev in zip(a[4:],e[4:])),(sql,a,e)

with tempfile.TemporaryDirectory(prefix='adversarial-sql-state-',dir=ROOT/'tmp') as tmp:
 db=DB(pathlib.Path(tmp))
 try:
  names=['t','`select``中`']
  tables={name:{} for name in names}
  for i,name in enumerate(names):
   db.q('CREATE TABLE '+name+'(n BIGINT,v VECTOR(3),s VARCHAR(12),id BIGINT PRIMARY KEY)')
   rows={k:(R.randrange(-5,6),tuple(R.randrange(-2,3) for _ in range(3)),R.choice(['','a','aa','a\x00','a\t','a\n','中文','😀',"'",'\\'])) for k in list(range(-145,145))+[-2**63,2**63-1]}
   tables[name]=rows
   db.q('INSERT INTO '+name+' VALUES '+','.join(row_sql(k,r) for k,r in rows.items()))
   db.q('CREATE VECTOR INDEX `ix``中` ON '+name+'(v)')
   db.q('CREATE VECTOR INDEX second ON '+name+'(v)')
   for kind in ('near','far','multi','text'):
    for lim in (0,1,64,65,291,292,293,2**63-1,None):query(db,name,rows,kind,lim,mode='ann' if lim==1 else 'exact')
   for bad in ('LIMIT -1','LIMIT 9223372036854775808','LIMIT 1.0'):
    db.q('SELECT id FROM '+name+' '+bad,False)
  # Repeated rollback/recreate reuses catalog/index identifiers; no stale snapshots.
  for i in range(8):
   db.q('BEGIN');db.q('CREATE TABLE transient(id BIGINT PRIMARY KEY,v VECTOR(3))')
   db.q('INSERT INTO transient VALUES(99,[0,0,0])');db.q('CREATE VECTOR INDEX transient_ix ON transient(v)')
   for _ in range(3): assert db.q('SELECT id FROM transient ORDER BY L2_DISTANCE(v,[0,0,0]) LIMIT 1')['rows']==[[99]]
   db.q('ROLLBACK'); db.q('SELECT id FROM transient',False)
   for name in names:
    for _ in range(3):query(db,name,tables[name],lim=7)
  for step in range(48):
   name=names[step%2]; rows=tables[name]; old=rows.copy();db.q('BEGIN')
   key=1000+step
   value=(step%4,(0,0,0),'changed')
   db.q('INSERT INTO '+name+' VALUES '+row_sql(key,value));rows[key]=value
   # A valid prefix plus later collision/type failure cannot change any row.
   for suffix in [row_sql(next(iter(rows)),value),row_sql(3000+step,(0,(1,2),'bad')),row_sql(3000+step,(0,(1,2,3),'too long text!'))]:
    db.q('INSERT INTO '+name+' VALUES '+row_sql(2000+step,value)+','+suffix,False)
    assert db.q('SELECT id FROM '+name+' WHERE id='+str(2000+step))['rows']==[]
   db.q('UPDATE '+name+' SET id=77 WHERE id='+str(key),False)
   db.q('CREATE VECTOR INDEX second ON '+name+'(v)',False)
   db.q('CREATE VECTOR INDEX bad ON '+name+'(s)',False)
   if step%3==0:
    db.q('UPDATE '+name+' SET v=[1,-1,0],n=9,s=\'upd\' WHERE id>=-100 AND id<=100')
    for k in rows:
     if -100<=k<=100:rows[k]=(9,(1,-1,0),'upd')
   elif step%3==1:
    db.q('DELETE FROM '+name+' WHERE n>=2 AND s<\'😀\'')
    rows={k:r for k,r in rows.items() if not(r[0]>=2 and r[2]<'😀')};tables[name]=rows
   else:
    db.q('UPDATE '+name+' SET n=-2 WHERE id=0 AND id=1')
    db.q('DELETE FROM '+name+' WHERE id=0 AND id=1')
   for _ in range(3):query(db,name,rows,lim=7,q=(step%3,0,-1))
   for kind in ('multi','text','far'):
    query(db,name,rows,kind,lim=67,pred=('n>=0 AND s<=\'中文😀\'',lambda k,r:r[0]>=0 and r[2]<='中文😀'))
   if step%2:db.q('COMMIT')
   else:db.q('ROLLBACK');tables[name]=old;rows=old
   for _ in range(3):query(db,name,rows,lim=7)
   other=names[(step+1)%2];query(db,other,tables[other],lim=5)
   if step%12==0:
    db.restart()
    for table in names:query(db,table,tables[table],'multi',None)
  # Shutdown inside a transaction discards data and catalog work.
  db.q('BEGIN');db.q('DELETE FROM '+names[0]);db.q('CREATE TABLE lost(id BIGINT PRIMARY KEY)');db.restart()
  db.q('SELECT * FROM lost',False)
  for name in names:query(db,name,tables[name],'multi',None)
  # Quoted keyword/function/embedded delimiter names and ORDER BY aliases.
  db.q('CREATE TABLE `table;--`(`primary` BIGINT PRIMARY KEY,`l2_distance` BIGINT,`v``x` VECTOR(3))')
  db.q('INSERT INTO `table;--` VALUES(2,6,[1,0,0]),(1,9,[0,0,0])')
  assert db.q('SELECT `primary`,`l2_distance` AS `from;`,L2_DISTANCE(`v``x`,[0,0,0]) AS `order` FROM `table;--` ORDER BY `from;`')['rows']==[[2,6,1],[1,9,0]]
  assert db.q('SELECT `primary` AS `l2_distance` FROM `table;--` ORDER BY `l2_distance`')['rows']==[[1],[2]]
  db.close()
 except Exception:
  (ROOT/'work/adversarial-sql-failure.json').write_text(json.dumps(transcript,ensure_ascii=False,indent=2))
  if db.p and db.p.poll() is None:db.p.kill();db.p.wait(timeout=10)
  raise
result={'status':'PASS','seed':548219,'checks':checks,'rows_initial':584,'transaction_cycles':56,'coverage':['4096-byte spill sort','multi-distance expressions','distance ties','INT64 extreme keys','LIMIT extremes','Unicode/control VARCHAR','quoted identifiers','2 indexes/table','cross-table snapshots','index/table rollback/recreation','failed multirow inserts','failed DDL','snapshot invalidation','shutdown rollback','reopen']}
(ROOT/'work/adversarial-sql-state-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
