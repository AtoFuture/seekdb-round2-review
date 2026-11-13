"""Interactive strict-JSONL acceptance tests, including recovery and response spooling."""
import json, os, pathlib, select, subprocess, sys, tempfile, unittest
BINARY = pathlib.Path(sys.argv.pop(1)).resolve()
ROOT = pathlib.Path(os.environ.get('MVDB_TEST_DIR', '/data/core-tests'))
ROOT.mkdir(parents=True, exist_ok=True)

def strict(s):
    def pairs(items):
        d = {}
        for k,v in items:
            assert k not in d, 'duplicate response key'
            d[k] = v
        return d
    return json.loads(s, object_pairs_hook=pairs, parse_constant=lambda s: (_ for _ in ()).throw(ValueError(s)))

class JsonlTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='jsonl-', dir=ROOT)
        self.data = pathlib.Path(self.temp.name)
        self.proc = None
    def tearDown(self):
        self.stop()
        self.temp.cleanup()
    def stop(self):
        if self.proc:
            if self.proc.poll() is None: self.proc.kill()
            self.proc.wait(timeout=10)
            for p in (self.proc.stdin,self.proc.stdout,self.proc.stderr): p.close()
            self.proc = None
    def start(self, preexec_fn=None):
        self.proc = subprocess.Popen([str(BINARY),'--data-dir',str(self.data)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,preexec_fn=preexec_fn)
        self.assertEqual(self.read(), {'type':'ready'})
    def read(self):
        self.assertTrue(select.select([self.proc.stdout],[],[],8)[0], 'response timeout')
        line = self.proc.stdout.readline()
        self.assertTrue(line, 'process closed stdout before response')
        return strict(line)
    def raw(self, text):
        self.proc.stdin.write(text.encode()+b'\n'); self.proc.stdin.flush()
        return self.read()
    def query(self, sql, ident=1, mode='exact', ok=True):
        r=self.raw(json.dumps(dict(id=ident,op='execute',sql=sql,search_mode=mode)))
        self.assertEqual(type(r['id']), type(ident)); self.assertEqual(r['id'],ident)
        self.assertIs(r['ok'],ok)
        if not ok: self.assertTrue(r['error']['code'])
        return r
    def shutdown(self):
        self.assertEqual(self.raw('{"id":999,"op":"shutdown"}'),{'id':999,'ok':True})
        self.proc.wait(timeout=8)
        self.assertEqual(self.proc.stdout.read(), b'')
        self.assertEqual(self.proc.returncode,0)
        self.stop()
    def test_types_unicode_errors_shutdown_recovery(self):
        self.start()
        self.query('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(100),v VECTOR(2));')
        text='中文😀; quote" slash\\ tab\t newline\n'
        self.query("INSERT INTO t VALUES(-9223372036854775808,'"+text+"',[3,4]),(9223372036854775807,'max',[0,0]);", ident=9223372036854775807)
        for ident in [-(2**63),2**64-1,2**80,'id😀',None,True,1.25,{'nested':[1,'x']}]:
            r=self.query('SELECT id,s,v FROM t ORDER BY id;',ident,mode='ann')
            self.assertEqual(r['rows'],[[-2**63,text,[3,4]],[2**63-1,'max',[0,0]]])
        self.query("INSERT INTO t VALUES(9223372036854775807,'bad',[1,2]);",ok=False)
        self.query('BEGIN;'); self.query('DELETE FROM t;')
        self.shutdown(); self.start()
        self.assertEqual(len(self.query('SELECT id FROM t;')['rows']),2)
        self.shutdown()
    def test_invalid_requests_continue_and_empty_columns(self):
        self.start()
        for raw in [' {"id":1e999,"op":"shutdown"}', '{"id":[1e999],"op":"shutdown"}', '{"id":1,"id":2,"op":"shutdown"}', '{"id":NaN}', '{"id":1,"op":"execute","sql":"\\ud800"}', '{}', '[]', '{"id":1,"op":"execute","sql":"x","search_mode":"bad"}']:
            self.assertIs(self.raw(raw)['ok'],False)
        self.query('CREATE TABLE t(id BIGINT PRIMARY KEY);')
        r=self.query('SELECT id FROM t;'); self.assertEqual(r['columns'],['id']); self.assertEqual(r['rows'],[])
        self.shutdown()
    def test_large_response_and_crash_rollback(self):
        self.start(); self.query('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2000));')
        payload='x'*1500
        self.query('INSERT INTO t VALUES '+','.join("(%d,'%s')"%(i,payload) for i in range(100))+';')
        self.assertEqual(len(self.query('SELECT s FROM t;')['rows']),100)
        self.assertFalse(list(self.data.glob('.jsonl-*')))
        self.query('BEGIN;'); self.query('DELETE FROM t;'); self.stop(); self.start()
        self.assertEqual(len(self.query('SELECT id FROM t;')['rows']),100)
        self.shutdown()
    def test_repeated_spooled_small_and_error_responses(self):
        self.start()
        self.query('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2000));')
        payload='r'*1500
        self.query('INSERT INTO t VALUES '+','.join("(%d,'%s')"%(i,payload) for i in range(60))+';')
        for round in range(12):
            rows=self.query('SELECT s FROM t ORDER BY id;',ident={'round':round})['rows']
            self.assertEqual(rows,[[payload] for _ in range(60)])
            self.query('SELECT unknown FROM t;',ident='error-'+str(round),ok=False)
            self.assertEqual(self.query('SELECT id FROM t WHERE id=17;')['rows'],[[17]])
            self.assertEqual(self.query('SELECT id FROM t WHERE id<0;')['rows'],[])
            self.assertFalse(list(self.data.glob('.jsonl-*')))
        self.shutdown()
    def test_spool_failure_is_one_error_then_session_continues(self):
        import resource, signal
        self.start(); self.query('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2000));')
        self.query('INSERT INTO t VALUES '+','.join("(%d,'%s')"%(i,'x'*1500) for i in range(100))+';')
        self.shutdown()
        def restrict_spool():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (65536,65536))
        self.start(preexec_fn=restrict_spool)
        self.query('SELECT s FROM t;',ok=False)
        self.assertEqual(len(self.query('SELECT id FROM t;')['rows']),100)
        self.assertFalse(list(self.data.glob('.jsonl-*')))
        self.shutdown()
if __name__=='__main__': unittest.main()
