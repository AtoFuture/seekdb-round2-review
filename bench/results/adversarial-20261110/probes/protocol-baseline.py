import json, pathlib, select, subprocess, resource, time
ROOT=pathlib.Path('/mnt/d/Codex/MiniVectorDB/work/adversarial-protocol-20261110'); ROOT.mkdir(exist_ok=True)
BINARY='/mnt/d/Codex/MiniVectorDB/build/row-cpu-core/minivectordb'
results=[]
class DB:
 def __init__(self,name,limit=None):
  self.data=ROOT/name; self.data.mkdir(exist_ok=True)
  def restrict():
   if limit is not None: resource.setrlimit(resource.RLIMIT_NOFILE,(limit,limit))
  self.p=subprocess.Popen([BINARY,'--data-dir',str(self.data)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,preexec_fn=restrict if limit else None)
  assert self.read()=={'type':'ready'}
 def read(self):
  assert select.select([self.p.stdout],[],[],5)[0], 'timeout'
  line=self.p.stdout.readline(); assert line, (self.p.poll(),self.p.stderr.read())
  return json.loads(line,parse_constant=lambda s: (_ for _ in ()).throw(ValueError(s)))
 def raw(self,s):
  self.p.stdin.write((s.encode() if isinstance(s,str) else s)+b'\n'); self.p.stdin.flush(); return self.read()
 def q(self,s,id=1,**kw): return self.raw(json.dumps(dict(id=id,op='execute',sql=s,**kw),ensure_ascii=True))
 def stop(self):
  assert self.raw('{"id":99,"op":"shutdown"}')==dict(id=99,ok=True)
  assert self.p.wait(timeout=5)==0; assert self.p.stdout.read()==b''
def record(label,actual,expected):
 ok=actual==expected
 results.append(dict(label=label,passed=ok,actual=actual if len(repr(actual))<500 else repr(actual)[:500],expected=expected if len(repr(expected))<500 else repr(expected)[:500]))
 if not ok: print('FAIL',label,repr(actual)[:200],repr(expected)[:200],flush=True)
db=DB('matrix-'+str(time.time_ns()))
record('create',db.q('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(128),v VECTOR(2));')['ok'],True)
malformed=[b'',b'null',b'[]',b'{',b'{}x',b'{"id":0,"id":1,"op":"shutdown"}',b'{"id":0,"op":"shutdown",}',b'{"id":01}',b'{"id":+1}',b'{"id":.1}',b'{"id":1.}',b'{"id":1e}',b'{"id":NaN}',b'{"id":Infinity}',b'{"id":1e309}',b'{"id":"\\ud800"}',b'{"id":"\\udc00"}',b'{"id":"\\ud800\\u0000"}',b'{"id":"\\x00"}',b'{"id":"\x00"}',b'{"id":"\xc0\xaf"}',b'{"id":"\xed\xa0\x80"}',b'{"id":"\xf4\x90\x80\x80"}',b'{"id":"\xff"}',b'{}\x00',b'{"id":2,"op":null}',b'{"id":2,"op":"execute","sql":[]}',b'{"id":2,"op":"execute","sql":"SELECT id FROM t;","search_mode":0}',b'{"id":1,"op":"execute","sql":"SELECT id FROM t;","metadata":{"k":1,"k":2}}']
for i,s in enumerate(malformed):
 record('bad frame '+str(i),db.raw(s)['ok'],False)
 record('after bad frame '+str(i),db.q('SELECT id FROM t;')['rows'],[])
ids=[None,False,True,0,-0.0,1.25,-1.5,2**1000,-2**1000,'',"id\x00\t\b\f\r\n\\/\"😀\u2028\u2029",{'n':[None,True,False,-2**100,'\x00😀']},['a',{},[],1]]
for i,ident in enumerate(ids):
 ans=db.q('SELECT id FROM t;',ident)
 record('id value '+str(i),ans.get('id'),ident);record('id type '+str(i),type(ans.get('id')).__name__,type(ident).__name__)
for depth in [0,1,8,32,63,64,65,100,1000]:
 raw='{"id":1,"op":"execute","sql":"SELECT id FROM t;","unknown":'+'['*depth+'0'+']'*depth+'}'
 record('depth '+str(depth),db.raw(raw)['ok'],depth<=64)
 record('after depth '+str(depth),db.q('SELECT id FROM t;')['rows'],[])
for i,value in enumerate(["'",'\\','--semicolon;/*x*/','中文😀','e\u0301','\x00','\n\r\t\b\f',chr(0x10ffff),chr(0xfffe),chr(0xffff),'\u2028\u2029']):
 sql="INSERT INTO t VALUES(%d,'%s',[+1.e0,-.5e+1]);"%(i,value.replace("'","''"))
 record('text insert '+str(i),db.q(sql)['ok'],True)
 record('text roundtrip '+str(i),db.q('SELECT s,v FROM t WHERE id=%d;'%i)['rows'],[[value,[1,-5]]])
validsql=["/*lead*/SELECT/*mid*/id FROM t WHERE id=0;--tail", "SELECT id FROM t WHERE id=0 --comment\n;", "SELECT id FROM t WHERE id=+000;", "SELECT `ID` FROM `T` WHERE `ID`=0;", "SELECT id FROM t WHERE id=0 AND id>=-9223372036854775808 AND id<=9223372036854775807;"]
for i,s in enumerate(validsql):record('valid lexer '+str(i),db.q(s).get('rows'),[[0]])
badsql=["INSERT INTO t VALUES(999,'bad',[1,]);","INSERT INTO t VALUES(999,'bad',[1e,2]);","INSERT INTO t VALUES(999,'bad',[+-,2]);","INSERT INTO t VALUES(999,'bad','[1,2]; SELECT id FROM t;');","INSERT INTO t VALUES(999,'bad',[1e999,2]);","INSERT INTO t VALUES(999,'bad',[1,2]); DELETE FROM t;","INSERT INTO t VALUES(999,'bad',[1,2]) /*", "INSERT INTO t VALUES(999,'bad',[1,2]) trailing", "INSERT INTO t VALUES(9223372036854775808,'bad',[1,2]);"]
for i,s in enumerate(badsql):
 record('invalid sql '+str(i),db.q(s)['ok'],False)
 record('sql failure atomicity '+str(i),db.q('SELECT id FROM t WHERE id=999;')['rows'],[])
for i,name in enumerate(['semi;name','tick`name','line\nname','\x00name','中文😀','select']):
 escaped='`'+name.replace('`','``')+'`'
 record('quoted table '+str(i),db.q('CREATE TABLE '+escaped+'(id BIGINT PRIMARY KEY);')['ok'],True)
 record('quoted table select '+str(i),db.q('SELECT id FROM '+escaped+';').get('rows'),[])
db.stop()
for limit in [4,5,6,7]:
 name='spool-fd-'+str(limit)+'-'+str(time.time_ns())
 setup=DB(name); setup.q('CREATE TABLE t(id BIGINT PRIMARY KEY);'); setup.stop()
 try: fault=DB(name,limit)
 except Exception as e:
  results.append(dict(label='fd start '+str(limit),diagnostic=repr(e)));continue
 created=fault.q('CREATE TABLE t(id BIGINT PRIMARY KEY);')
 for length in [65490,65510,65515,65520,65525]:
  ans=fault.q('INSERT INTO t VALUES(%d);'%length,'x'*length)
  seen=fault.q('SELECT id FROM t WHERE id=%d;'%length)
  results.append(dict(label='spool mutation fd=%d idlen=%d'%(limit,length),ok=ans.get('ok'),error=ans.get('error'),rows=seen.get('rows'),passed=ans.get('ok') or seen.get('rows')==[]))
 fault.stop()
(ROOT/'results.json').write_text(json.dumps(results,ensure_ascii=True,indent=2)+'\n')
print(json.dumps(dict(total=len(results),failures=[r for r in results if r.get('passed') is False]),indent=2))


