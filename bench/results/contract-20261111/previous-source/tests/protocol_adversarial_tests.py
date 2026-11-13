"""Adversarial JSONL framing and acknowledgement/state consistency checks."""
import json
import os
import pathlib
import resource
import select
import subprocess
import sys
import tempfile
import unittest

BINARY = pathlib.Path(sys.argv.pop(1)).resolve()
ROOT = pathlib.Path(os.environ.get('MVDB_TEST_DIR', '/data/core-tests'))
ROOT.mkdir(parents=True, exist_ok=True)


def strict_json(line):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate response key')
            result[key] = value
        return result
    def reject_constant(value):
        raise ValueError('non-finite JSON constant: ' + value)
    return json.loads(line, object_pairs_hook=pairs, parse_constant=reject_constant)


class AdversarialProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='protocol-adversarial-', dir=ROOT)
        self.data = pathlib.Path(self.temp.name)
        self.proc = None

    def tearDown(self):
        self.stop()
        self.temp.cleanup()

    def stop(self):
        if self.proc:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=8)
            for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
                stream.close()
            self.proc = None

    def start(self, no_spool=False):
        def restrict_descriptors():
            # stdin/stdout/stderr and the existing database occupy all four FDs.
            resource.setrlimit(resource.RLIMIT_NOFILE, (4, 4))
        self.proc = subprocess.Popen(
            [str(BINARY), '--data-dir', str(self.data)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            preexec_fn=restrict_descriptors if no_spool else None)
        self.assertEqual(self.read(), {'type': 'ready'})

    def read(self):
        self.assertTrue(select.select([self.proc.stdout], [], [], 8)[0], 'response timeout')
        line = self.proc.stdout.readline()
        self.assertTrue(line, 'database exited before response')
        self.assertTrue(line.endswith(b'\n'))
        self.last_response = line
        return strict_json(line)

    def raw(self, request):
        wire = request.encode('utf-8') if isinstance(request, str) else request
        self.proc.stdin.write(wire + b'\n')
        self.proc.stdin.flush()
        return self.read()

    def query(self, sql, ident=1, ok=True):
        response = self.raw(json.dumps(dict(id=ident, op='execute', sql=sql)))
        self.assertEqual(type(response.get('id')), type(ident))
        self.assertEqual(response.get('id'), ident)
        self.assertIs(response.get('ok'), ok, repr(response.get('error')))
        if not ok:
            self.assertTrue(response['error']['code'])
        return response

    def shutdown(self, ident=999):
        response = self.raw(json.dumps(dict(id=ident, op='shutdown')))
        self.assertEqual(response, dict(id=ident, ok=True))
        self.assertEqual(self.proc.wait(timeout=8), 0)
        self.assertEqual(self.proc.stdout.read(), b'')
        self.stop()

    def initialize(self, schema='id BIGINT PRIMARY KEY'):
        self.start()
        self.query('CREATE TABLE t(' + schema + ');')
        self.shutdown()

    def test_long_ids_do_not_change_mutation_or_commit_outcome(self):
        self.initialize()
        self.start(no_spool=True)
        lengths = [65490, 65510, 65515, 65520, 65525]
        for key, length in enumerate(lengths):
            with self.subTest(id_length=length):
                response = self.query('INSERT INTO t VALUES(%d);' % key, 'x' * length)
                self.assertEqual(response['affected_rows'], 1)
                self.assertEqual(self.query('SELECT id FROM t WHERE id=%d;' % key)['rows'], [[key]])
        self.query('BEGIN;', 'b' * 65518)
        self.query('INSERT INTO t VALUES(100);')
        self.query('COMMIT;', 'c' * 65518)
        self.shutdown()
        self.start(no_spool=True)
        self.assertEqual(self.query('SELECT id FROM t ORDER BY id;')['rows'], [[i] for i in range(5)] + [[100]])
        self.shutdown()

    def test_long_shutdown_ack_rolls_back_and_exits(self):
        self.initialize()
        self.start(no_spool=True)
        self.query('BEGIN;')
        self.query('INSERT INTO t VALUES(10);')
        self.shutdown('s' * 65518)
        self.start(no_spool=True)
        self.assertEqual(self.query('SELECT id FROM t;')['rows'], [])
        self.shutdown()

    def test_failed_statement_retains_earlier_transaction_work(self):
        self.initialize()
        self.start(no_spool=True)
        self.query('BEGIN;')
        self.query('INSERT INTO t VALUES(1);')
        ident = {'escaped': '\x00\n\"\\😀', 'items': ['x' * 65510, 2**100, None]}
        self.query('INSERT INTO t VALUES(2),(1);', ident, ok=False)
        self.assertEqual(self.query('SELECT id FROM t ORDER BY id;')['rows'], [[1]])
        self.query('COMMIT;', ident)
        self.shutdown()
        self.start(no_spool=True)
        self.assertEqual(self.query('SELECT id FROM t;')['rows'], [[1]])
        self.shutdown()

    def test_row_spool_failure_is_one_error_and_session_continues(self):
        self.initialize('id BIGINT PRIMARY KEY, s VARCHAR(2000)')
        self.start()
        self.query('INSERT INTO t VALUES' + ','.join("(%d,'%s')" % (i, 'x' * 1500) for i in range(50)) + ';')
        self.shutdown()
        self.start(no_spool=True)
        self.query('SELECT s FROM t;', ok=False)
        self.assertEqual(self.query('SELECT id FROM t ORDER BY id;')['rows'], [[i] for i in range(50)])
        self.assertFalse(list(self.data.glob('.jsonl-*')))
        self.shutdown()

    def test_malformed_frames_then_valid_requests_and_unicode_roundtrip(self):
        self.initialize('id BIGINT PRIMARY KEY, s VARCHAR(128), v VECTOR(2)')
        self.start()
        malformed = [b'', b'null', b'{', b'{"id":01}', b'{"id":1e309}',
                     b'{"id":1,"id":2}', b'{"id":"\\ud800"}',
                     b'{"id":"\xc0\xaf"}', b'{"id":"\xed\xa0\x80"}',
                     b'{"id":1,"op":"execute","sql":[]}']
        for wire in malformed:
            with self.subTest(frame=wire):
                self.assertIs(self.raw(wire)['ok'], False)
                self.assertEqual(self.query('SELECT id FROM t;')['rows'], [])
        for depth in [32, 64, 65, 1000]:
            wire = ('{"id":1,"op":"execute","sql":"SELECT id FROM t;","unknown":' +
                    '[' * depth + '0' + ']' * depth + '}')
            self.assertIs(self.raw(wire)['ok'], depth <= 64)
            self.assertEqual(self.query('SELECT id FROM t;')['rows'], [])
        text = "中文😀'\x00\n\r\t\\--/*;*/e\u0301"
        self.query("INSERT INTO t VALUES(+000,'%s',[+1.e0,-.5e+1]);" % text.replace("'", "''"))
        for ident in [None, True, -0.0, 2**100, 'id\x00\n😀', {'nested': [False, 2**100, '\\"']}]:
            response = self.query('/*lead*/SELECT s,v FROM `T` WHERE `ID`=0;--tail', ident)
            self.assertEqual(response['rows'], [[text, [1, -5]]])
        self.query("INSERT INTO t VALUES(999,'bad',[1,2]); DELETE FROM t;", ok=False)
        self.assertEqual(self.query('SELECT id FROM t;')['rows'], [[0]])
        self.shutdown()

    def test_raw_id_spelling_and_large_id_with_successful_row_spool(self):
        self.initialize('id BIGINT PRIMARY KEY, s VARCHAR(2000)')
        self.start()
        self.query('INSERT INTO t VALUES' + ','.join("(%d,'%s')" % (i, 'x' * 1500) for i in range(50)) + ';')
        for ident in ['-0', '1e+02', '1.2500', '{"nested" : [1, true, "x"]}']:
            response = self.raw('{"id":' + ident + ',"op":"execute","sql":"SELECT id FROM t LIMIT 0"}')
            self.assertTrue(response['ok'])
            self.assertTrue(self.last_response.startswith(('{"id":' + ident + ',').encode()))
        ident = 'id\x00\n😀' + 'x' * 100000
        response = self.query('SELECT id,s FROM t ORDER BY id;', ident)
        self.assertEqual(response['rows'], [[i, 'x' * 1500] for i in range(50)])
        self.assertFalse(list(self.data.glob('.jsonl-*')))
        self.shutdown()


if __name__ == '__main__':
    unittest.main()
