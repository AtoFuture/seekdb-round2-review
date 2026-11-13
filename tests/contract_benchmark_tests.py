"""Contract benchmark helper regressions; no database or Docker required."""
import importlib.util
import math
import os
import re
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / "bench" / "run_contract_benchmark.py"
spec = importlib.util.spec_from_file_location("contract_benchmark", PATH)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ContractBenchmarkTests(unittest.TestCase):
    def test_paged_verification_checks_every_row_and_terminal_page(self):
        count = 10001
        w = {"vectors": [[0.0]] * count, "group_values": [0] * count, "bodies": ["x"] * count}
        records = [[i, 0, [0.0], "x"] for i in range(count)]
        calls = []
        def request(sql):
            calls.append(sql)
            match = re.search(r"WHERE id>(-?\d+)", sql)
            last = int(match[1]) if match else -1
            rows = [row for row in records if row[0] > last][:1000]
            return {"ok": True, "columns": ["id", "g", "v", "body"], "rows": rows}, 0
        bench.verify_stored_rows(request, w)
        self.assertEqual(len(calls), 12)
        self.assertIn("id>10000", calls[-1])
        for broken in (records[:-1], records + [[count, 0, [0.0], "x"]],
                       records[:999] + records[998:]):
            original, records = records, broken
            with self.assertRaises(AssertionError):
                bench.verify_stored_rows(request, w)
            records = original

    def test_paged_verification_rejects_wrong_columns(self):
        w = {"vectors": [None] * 10001}
        with self.assertRaises(AssertionError):
            bench.verify_stored_rows(lambda sql: ({"ok": True, "columns": [], "rows": []}, 0), w)

    def test_io_requires_exact_device_and_both_counters(self):
        parsed = bench.parse_io("8:1 rbytes=11 wbytes=17 rios=1 wios=2\n8:2 rbytes=19 wbytes=23", "8:1")
        self.assertEqual(parsed, {"rbytes": 11, "wbytes": 17})
        for source in ("8:2 rbytes=1 wbytes=2", "8:1 rbytes=1", "8:1 rbytes=1 wbytes=-1", ""):
            with self.subTest(source=source), self.assertRaises(ValueError):
                bench.parse_io(source, "8:1")

    def test_memory_fields_must_exist(self):
        with self.assertRaises(ValueError):
            bench.parse_memory("file_dirty 0")
        self.assertEqual(bench.parse_memory("file_dirty 12\nfile_writeback 34"), {"file_dirty": 12, "file_writeback": 34})

    def test_device_uses_stat_device_not_assumed_drive(self):
        with tempfile.TemporaryDirectory() as directory:
            actual = os.stat(directory).st_dev
            self.assertEqual(bench.data_device(Path(directory)), f"{os.major(actual)}:{os.minor(actual)}")

    def test_owned_tree_drain_syncs_regular_files_and_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "nested").mkdir()
            (root / "nested" / "page").write_bytes(b"data")
            (root / "root").write_bytes(b"root")
            self.assertEqual(bench.fsync_owned_tree(root), {"files": 2, "directories": 2})

    def test_owned_tree_rejects_external_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "owned").mkdir()
            (root / "outside").write_bytes(b"do not touch")
            (root / "owned" / "link").symlink_to(root / "outside")
            with self.assertRaises(ValueError):
                bench.fsync_owned_tree(root / "owned")

    def test_owned_root_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "owned").mkdir()
            (root / "alias").symlink_to(root / "owned", target_is_directory=True)
            with self.assertRaises((ValueError, OSError)):
                bench.fsync_owned_tree(root / "alias")

    def test_owned_tree_rejects_hardlinked_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "owned").mkdir()
            (root / "outside").write_bytes(b"outside")
            os.link(root / "outside", root / "owned" / "linked")
            with self.assertRaises(ValueError):
                bench.fsync_owned_tree(root / "owned")

    def test_drain_requires_clean_and_two_stable_observations(self):
        def sample(dirty, writes):
            return {"dirty": {"file_dirty": dirty, "file_writeback": 0},
                    "io": {"rbytes": 0, "wbytes": writes}}
        snapshots = iter([sample(10, 0), sample(0, 1), sample(0, 2), sample(0, 2), sample(0, 2)])
        class FakeResources:
            def sample(self):
                return next(snapshots)
        with tempfile.TemporaryDirectory() as directory, patch.object(bench.time, "sleep"):
            result = bench.drain(FakeResources(), Path(directory))
        self.assertEqual(len(result["samples"]), 4)
        self.assertEqual(result["before"]["dirty"]["file_dirty"], 10)
        self.assertEqual(result["after"]["io"]["wbytes"], 2)

    def test_drain_does_not_wait_forever_on_dirty_pages(self):
        class FakeResources:
            def sample(self):
                return {"dirty": {"file_dirty": 1, "file_writeback": 0},
                        "io": {"rbytes": 0, "wbytes": 1}}
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(TimeoutError):
            bench.drain(FakeResources(), Path(directory), timeout=0)

    def test_summary_retains_rounds_and_reports_metric_medians_without_score(self):
        samples = [dict(version="candidate", phase="write", indexed=False, round=i + 1,
                        T_ms=t, B_bytes=b, M_bytes=m)
                   for i, (t, b, m) in enumerate([(1, 90, 9), (4, 10, 3), (8, 50, 1)])]
        summary = bench.summarize(samples)
        group = summary["candidate/write/unindexed"]
        self.assertEqual(group["rounds"], [1, 2, 3])
        self.assertEqual(group["median"], {"T_ms": 4, "B_bytes": 50, "M_bytes": 3})
        self.assertEqual(group["samples"], samples)
        self.assertNotIn("E", group)
        self.assertNotIn("score", group)

    def test_query_group_metadata_records_odd_counts(self):
        even = bench.workload_metadata(20)
        odd = bench.workload_metadata(21)
        self.assertEqual((even["unfiltered_queries"], even["filtered_queries"]), (10, 10))
        self.assertTrue(even["half_filtered"])
        self.assertEqual((odd["unfiltered_queries"], odd["filtered_queries"]), (11, 10))
        self.assertFalse(odd["half_filtered"])

    def test_sql_input_hash_covers_schema_groups_and_payload(self):
        first = bench.make_workload(2, 2, 8, "bigint")
        other_type = bench.make_workload(2, 2, 8, "varchar")
        self.assertNotEqual(bench.sql_workload_hash(first), bench.sql_workload_hash(other_type))
        unchanged = bench.sql_workload_hash(first)
        first["inserts"][0] = first["inserts"][0].replace("record-", "changed-")
        self.assertNotEqual(bench.sql_workload_hash(first), unchanged)

    def test_cleanup_failure_cannot_leave_passed_true(self):
        class FailedCleanup:
            def close(self):
                raise OSError("cleanup failure")
        result = {"passed": True}
        with self.assertRaises(OSError):
            bench.close_session(FailedCleanup(), result)
        self.assertFalse(result["passed"])
        self.assertIn("cleanup failure", result["cleanup_error"])

    def test_filter_sql_and_truth_group_match_for_both_types(self):
        for kind in ("bigint", "varchar"):
            workload = bench.make_workload(23, 4, 20261110, kind)
            self.assertEqual(workload["groups"], [i % 10 for i in range(23)])
            self.assertNotIn(" WHERE ", workload["queries"][0])
            self.assertIn("WHERE g=" + ("0" if kind == "bigint" else "'category-0'"), workload["queries"][1])
            self.assertIn("WHERE g=" + ("1" if kind == "bigint" else "'category-1'"), workload["queries"][3])
            self.assertIn("g " + ("BIGINT" if kind == "bigint" else "VARCHAR(32)"), workload["create"])
            for j in (1, 3):
                independent_ids = [i for i in range(23) if i % 10 == (j // 2) % 10]
                self.assertEqual([i for i in range(23) if workload["group_values"][i] ==
                                  (j // 2) % 10] if kind == "bigint" else
                                 [i for i in range(23) if workload["group_values"][i] ==
                                  "category-" + str((j // 2) % 10)], independent_ids)

    def test_full_rows_and_topk_checks_are_exact_except_distance(self):
        w = bench.make_workload(2, 2, 8, "varchar")
        rows = [[i, w["group_values"][i], w["vectors"][i], w["bodies"][i]] for i in range(2)]
        bench.verify_rows({"ok": True, "columns": ["id", "g", "v", "body"], "rows": rows}, w)
        rows[0][1] = "bad"
        with self.assertRaises(AssertionError):
            bench.verify_rows({"ok": True, "columns": ["id", "g", "v", "body"], "rows": rows}, w)


if __name__ == "__main__":
    unittest.main()

