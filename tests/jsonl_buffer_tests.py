#!/usr/bin/env python3
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
LIMIT = 16 * 1024 * 1024

class BufferedFrames(unittest.TestCase):
    def setUp(self):
        if os.environ.get('MVDB_TEST_DIR'):
            Path(os.environ['MVDB_TEST_DIR']).mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('MVDB_TEST_DIR'))
        self.process = subprocess.Popen([BINARY, '--protocol', 'jsonl', '--data-dir', self.tmp.name],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.pending = b''
        self.assertEqual(self.receive(), {'type': 'ready'})

    def tearDown(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        for stream in [self.process.stdin, self.process.stdout, self.process.stderr]:
            stream.close()
        self.tmp.cleanup()

    def receive(self):
        import time
        deadline = time.monotonic() + 10
        while b'\n' not in self.pending:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, 'JSON response timeout')
            self.assertTrue(select.select([self.process.stdout], [], [], remaining)[0], 'missing flush/newline')
            data = os.read(self.process.stdout.fileno(), 65536)
            self.assertTrue(data, 'premature EOF')
            self.pending += data
        line, self.pending = self.pending.split(b'\n', 1)
        return json.loads(line)

    def send(self, data):
        while data:
            written = os.write(self.process.stdin.fileno(), data)
            data = data[written:]

    def request(self, ident, sql):
        return json.dumps({'id': ident, 'op': 'execute', 'sql': sql}, ensure_ascii=False).encode()

    def test_fragmented_unicode_pipelined_and_immediate_response(self):
        wire = self.request('初😀', 'CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64));') + b'\n'
        for byte in wire:
            self.send(bytes([byte]))
        self.assertEqual(self.receive()['id'], '初😀')
        rows = [self.request(i, "INSERT INTO t VALUES(%d,'😀');" % i) for i in range(3)]
        self.send(b'\r\n'.join(rows) + b'\r\n')
        for i in range(3):
            reply = self.receive();self.assertTrue(reply['ok']);self.assertEqual(reply['id'], i)
        # An open pipe after the complete request must not delay its response.
        self.send(self.request(9, 'SELECT id,s FROM t ORDER BY id;') + b'\n')
        self.assertEqual(self.receive()['rows'], [[i, '😀'] for i in range(3)])

    def test_exact_limit_oversize_drain_and_following_frame(self):
        wire = self.request(1, 'CREATE TABLE t(id BIGINT PRIMARY KEY);')
        self.send(wire + b' ' * (LIMIT-len(wire)) + b'\n')
        self.assertTrue(self.receive()['ok'])
        self.send(b' ' * (LIMIT+1) + b'\n' + self.request(2, 'SELECT id FROM t;') + b'\n')
        self.assertFalse(self.receive()['ok'])
        reply = self.receive();self.assertTrue(reply['ok']);self.assertEqual(reply['id'], 2)

    def test_buffer_boundaries_and_partial_final_frame(self):
        self.send(self.request(1, 'CREATE TABLE t(id BIGINT PRIMARY KEY);') + b'\n')
        self.assertTrue(self.receive()['ok'])
        for length in [65535, 65536, 65537, 131071, 131072]:
            wire = self.request(length, 'SELECT id FROM t;')
            self.send(wire + b' ' * (length-len(wire)) + b'\n')
            self.assertEqual(self.receive()['id'], length)
        self.send(self.request(7, 'SELECT id FROM t;'))
        self.process.stdin.close()
        self.assertEqual(self.receive()['id'], 7)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assertEqual(self.pending + self.process.stdout.read(), b'')

if __name__ == '__main__':
    unittest.main()
