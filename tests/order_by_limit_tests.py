#!/usr/bin/env python3
"""Differential test for ORDER BY ... LIMIT on a single ordinary column.

The bounded-heap path in the executor replaces the external sorter for a
single non-distance ORDER BY with a LIMIT. It must return exactly the same
rows in exactly the same order as the sorter path, including the rule that
ties are broken by ascending primary key in both directions.

Usage: order_by_limit_tests.py <path-to-minivectordb>
"""
import json
import os
import random
import subprocess
import sys
import tempfile

FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)


class Session:
    def __init__(self, binary, data_dir):
        self.process = subprocess.Popen(
            [binary, "--protocol", "jsonl", "--data-dir", data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)
        self.counter = 0
        ready = self.process.stdout.readline().strip()
        assert ready == '{"type":"ready"}', ready

    def execute(self, sql):
        self.counter += 1
        request = {"id": self.counter, "op": "execute", "sql": sql, "search_mode": "exact"}
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        response = json.loads(self.process.stdout.readline())
        assert response.get("id") == request["id"], response
        return response

    def close(self):
        try:
            self.counter += 1
            self.process.stdin.write(json.dumps({"id": self.counter, "op": "shutdown"}) + "\n")
            self.process.stdin.flush()
            self.process.stdout.readline()
        except Exception:
            pass
        try:
            self.process.stdin.close()
        except Exception:
            pass
        try:
            self.process.wait(timeout=30)
        except Exception:
            self.process.kill()


def oracle(rows, descending, limit):
    """Rows are (key, value). Ties are broken by ascending primary key.

    The sorter compares values with compare_cells and, for equal values, orders
    by ascending primary key in both directions.
    """
    import functools

    def compare(left, right):
        if left[1] < right[1]:
            return -1
        if left[1] > right[1]:
            return 1
        return 0

    def compare_desc(left, right):
        if left[1] > right[1]:
            return -1
        if left[1] < right[1]:
            return 1
        return 0

    def total(left, right):
        result = compare_desc(left, right) if descending else compare(left, right)
        if result:
            return result
        return (left[0] > right[0]) - (left[0] < right[0])

    ordered = sorted(rows, key=functools.cmp_to_key(total))
    return [key for key, _ in ordered[:limit]]


def main(binary):
    random.seed(20261112)
    data_dir = tempfile.mkdtemp(prefix="mvdb-order-by-")
    session = Session(binary, data_dir)
    try:
        rows = []
        for key in range(1, 601):
            value = random.randrange(0, 25)  # many ties on purpose
            rows.append((key, value))
        session.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, bucket BIGINT, tag VARCHAR(8));")
        values = ",".join(f"({key},{value},'g{value % 5}')" for key, value in rows)
        response = session.execute(f"INSERT INTO t VALUES {values};")
        check(response.get("ok"), f"insert failed: {response}")

        for limit in (1, 5, 10, 25, 100, 599, 600):
            for descending in (False, True):
                direction = "DESC" if descending else "ASC"
                response = session.execute(
                    f"SELECT id FROM t ORDER BY bucket {direction} LIMIT {limit};")
                check(response.get("ok"), f"order by bucket {direction} limit {limit}: {response}")
                got = [row[0] for row in response.get("rows", [])]
                expected = oracle(rows, descending, limit)
                check(got == expected,
                      f"bucket {direction} limit {limit}: expected {expected[:12]}... got {got[:12]}...")

        # A filter must not change the tie rule.
        for descending in (False, True):
            direction = "DESC" if descending else "ASC"
            response = session.execute(
                f"SELECT id FROM t WHERE bucket < 12 ORDER BY bucket {direction} LIMIT 7;")
            check(response.get("ok"), f"filtered order failed: {response}")
            filtered = [row for row in rows if row[1] < 12]
            got = [row[0] for row in response.get("rows", [])]
            expected = oracle(filtered, descending, 7)
            check(got == expected, f"filtered bucket {direction}: expected {expected} got {got}")

        # Ordering by the primary key in both directions.
        for descending in (False, True):
            direction = "DESC" if descending else "ASC"
            response = session.execute(f"SELECT id FROM t ORDER BY id {direction} LIMIT 9;")
            check(response.get("ok"), f"order by id {direction}: {response}")
            got = [row[0] for row in response.get("rows", [])]
            expected = oracle([(key, key) for key, _ in rows], descending, 9)
            check(got == expected, f"order by id {direction}: expected {expected} got {got}")

        # Multi column ordering still uses the general sorter and must agree.
        response = session.execute("SELECT id FROM t ORDER BY bucket ASC, id DESC LIMIT 8;")
        check(response.get("ok"), f"multi column order failed: {response}")
        got = [row[0] for row in response.get("rows", [])]
        expected = [key for key, _ in sorted(rows, key=lambda row: (row[1], -row[0]))[:8]]
        check(got == expected, f"multi column order: expected {expected} got {got}")

        # A VARCHAR ordering key exercises the text comparison path.
        response = session.execute("SELECT id FROM t ORDER BY tag ASC LIMIT 12;")
        check(response.get("ok"), f"order by tag failed: {response}")
        tagged = [(key, f"g{value % 5}") for key, value in rows]
        got = [row[0] for row in response.get("rows", [])]
        expected = oracle(tagged, False, 12)
        check(got == expected, f"order by tag: expected {expected} got {got}")

        # The projection of the ordering column must travel with the row.
        response = session.execute("SELECT id, bucket FROM t ORDER BY bucket DESC LIMIT 5;")
        check(response.get("ok"), f"projection order failed: {response}")
        pairs = [(row[0], row[1]) for row in response.get("rows", [])]
        expected_pairs = sorted(rows, key=lambda row: (-row[1], row[0]))[:5]
        check(pairs == expected_pairs, f"projection order: expected {expected_pairs} got {pairs}")
    finally:
        session.close()

    if FAILURES:
        for failure in FAILURES:
            print("FAIL:", failure)
        return 1
    print("order by limit: bounded heap matches the sorter order")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: order_by_limit_tests.py <binary>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
