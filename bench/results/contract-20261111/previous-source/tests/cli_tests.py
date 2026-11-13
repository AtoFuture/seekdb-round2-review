"""Black-box acceptance tests for the explicit local dev-tsv protocol."""
import os
import math
import random
import pathlib
import select
import subprocess
import sys
import tempfile
import unittest

BINARY = pathlib.Path(sys.argv.pop(1)).resolve()
ROOT = pathlib.Path(os.environ.get("MVDB_TEST_DIR", "/data/core-tests"))
ROOT.mkdir(parents=True, exist_ok=True)

class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cli-", dir=ROOT)
        self.data = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def args(self):
        return [str(BINARY), "--protocol", "dev-tsv", "--data-dir", str(self.data),
                "--sort-memory", "4096"]

    def run_sql(self, sql, status=0):
        result = subprocess.run(self.args(), input=sql, text=True, encoding="utf-8",
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def test_default_protocol_is_jsonl(self):
        result = subprocess.run([str(BINARY), "--data-dir", str(self.data)],
                                input=b'', capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'{"type":"ready"}\n')

    def test_crud_vector_and_reopen(self):
        lines = self.run_sql("""
            CREATE TABLE docs (id BIGINT PRIMARY KEY, label VARCHAR(80), v VECTOR(2));
            INSERT INTO docs VALUES (2,'two',[3,4]),(1,'one',[0,0]);
            SELECT id, L2_DISTANCE(v,[0,0]) AS distance FROM docs ORDER BY distance LIMIT 2;
            UPDATE docs SET label='updated' WHERE id=2;
            DELETE FROM docs WHERE id=1;
        """)
        self.assertIn("ROW\ti:1\td:0", lines)
        self.assertIn("ROW\ti:2\td:5", lines)
        self.assertIn("ROW\ti:2\ts:updated", self.run_sql("SELECT id,label FROM docs;"))

    def test_framing_comments_and_text(self):
        lines = self.run_sql("""
            -- ; this is a comment
            CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(80));
            /* ; block */ INSERT INTO t VALUES (1,'中文;it''s\tok\nline');
            SELECT s FROM t; -- trailing
        """)
        self.assertIn("ROW\ts:中文;it's\\tok\\nline", lines)
        self.assertFalse(any(line.startswith("ERROR") for line in lines))

    def test_statement_atomicity_and_eof_rollback(self):
        self.run_sql("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(20));")
        lines = self.run_sql("""
            BEGIN;
            INSERT INTO t VALUES (1,'kept');
            INSERT INTO t VALUES (2,'undo'),(1,'duplicate');
            SELECT id FROM t;
            COMMIT;
            BEGIN; INSERT INTO t VALUES (3,'not committed');
        """, 1)
        self.assertEqual([x for x in lines if x.startswith("ROW")], ["ROW\ti:1"])
        self.assertEqual([x for x in self.run_sql("SELECT id FROM t;") if x.startswith("ROW")],
                         ["ROW\ti:1"])

    def test_incomplete_frame_does_not_execute(self):
        self.run_sql("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(20));")
        self.run_sql("INSERT INTO t VALUES (1,'unterminated", 1)
        self.assertEqual(self.run_sql("SELECT id FROM t"), ["RESULT\tid", "END\t0"])
        self.run_sql("/* never closed", 1)

    def test_random_sql_against_independent_model(self):
        rng = random.Random(8191)
        rows = {}
        snapshot = None
        script = ["CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(2));"]
        expected = []
        had_error = False
        for step in range(90):
            key = rng.randrange(14)
            group = rng.randrange(3)
            vector = (rng.randrange(-5, 6), rng.randrange(-5, 6))
            operation = rng.randrange(6)
            if operation == 0:
                if snapshot is None:
                    script.append("BEGIN;")
                    snapshot = rows.copy()
                else:
                    commit = bool(rng.randrange(2))
                    script.append("COMMIT;" if commit else "ROLLBACK;")
                    if not commit:
                        rows = snapshot
                    snapshot = None
            elif operation in (1, 2):
                script.append(f"INSERT INTO t VALUES({key},{group},[{vector[0]},{vector[1]}]);")
                if key in rows:
                    had_error = True
                else:
                    rows[key] = (group, vector)
            elif operation == 3:
                script.append(f"UPDATE t SET v=[{vector[0]},{vector[1]}] WHERE g={group};")
                rows = {k: (g, vector if g == group else v) for k, (g, v) in rows.items()}
            elif operation == 4:
                script.append(f"DELETE FROM t WHERE id={key};")
                rows.pop(key, None)
            else:
                script.append(f"UPDATE t SET g={group} WHERE id={key};")
                if key in rows:
                    rows[key] = (group, rows[key][1])
            query = (rng.randrange(-2, 3), rng.randrange(-2, 3))
            script.append(f"SELECT id,L2_DISTANCE(v,[{query[0]},{query[1]}]) AS d "
                          f"FROM t WHERE g={group} ORDER BY d LIMIT 3;")
            ranked = sorted((sum((a-b)**2 for a, b in zip(v, query)), k)
                            for k, (g, v) in rows.items() if g == group)
            expected.append([(k, math.sqrt(distance)) for distance, k in ranked[:3]])
        if snapshot is not None:
            script.append("COMMIT;")
        output = self.run_sql("\n".join(script), 1 if had_error else 0)
        actual = []
        current = None
        for line in output:
            if line.startswith("RESULT"):
                current = []
            elif line.startswith("ROW"):
                cells = line.split("\t")[1:]
                current.append((int(cells[0][2:]), float(cells[1][2:])))
            elif line.startswith("END"):
                actual.append(current)
                current = None
        self.assertEqual(len(actual), len(expected))
        for observed, reference in zip(actual, expected):
            self.assertEqual([r[0] for r in observed], [r[0] for r in reference])
            for (_, a), (_, b) in zip(observed, reference):
                self.assertAlmostEqual(a, b, places=12)
        durable = self.run_sql("SELECT id FROM t ORDER BY id;")
        self.assertEqual([int(line.split("\t")[1][2:]) for line in durable if line.startswith("ROW")],
                         sorted(rows))
    def test_kill_keeps_commit_and_discards_transaction(self):
        self.run_sql("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(20));")
        proc = subprocess.Popen(self.args(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        try:
            for sql in [b"INSERT INTO t VALUES(1,'durable');\n", b"BEGIN;\n",
                        b"INSERT INTO t VALUES(2,'discard');\n"]:
                proc.stdin.write(sql)
                proc.stdin.flush()
                self.assertTrue(select.select([proc.stdout], [], [], 15)[0], "no SQL ack")
                self.assertTrue(proc.stdout.readline().startswith(b"OK\t"))
            proc.kill()
            proc.wait(timeout=10)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
            for pipe in [proc.stdin, proc.stdout, proc.stderr]:
                pipe.close()
        self.assertEqual([x for x in self.run_sql("SELECT id FROM t;") if x.startswith("ROW")],
                         ["ROW\ti:1"])

if __name__ == "__main__":
    assert BINARY.is_file(), f"SQL CLI executable is missing: {BINARY}"
    unittest.main()
