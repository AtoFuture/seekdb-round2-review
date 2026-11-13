# Optimization specification review

Verdict: PASS for the implementation specification; no blocking defect found. Performance benefit remains pending the isolated A/B experiment and is not implied by this verdict.

Reviewed 2026-11-09 against baseline 7f3c4ac624ac952ffc524d925fcfee24bec1e4ae, selective commit 2f0ba0afa33fa5942dc0bd6afc3bf6ae673b4f88, and current batch commit fa419bb. Repository was read-only throughout; only independent probe/report and test databases were created under D:/Codex/MiniVectorDB. No official test, push, branch change, source edit, or global WSL change.

## Verified requirements

- src/storage/row_codec.cpp:94 validates version, column count, field boundaries, all UTF-8/width constraints, finite float32 vector components, vector extent, and trailing bytes before predicates. operator[] at line 121 materializes/caches requested cells only. Validation does not silently skip unprojected/rejected-row data.
- src/sql/executor.cpp:118 and :129 keep LazyRowView inside a scan/get callback scope while backing bytes live. All SELECT branches use the wrapper, including lookup, unordered scan, exact TopK, sorter, and winner fetch. Eager decode remains available independently of batching.
- Materialization accounting is for constructed decoded Cells: eager full-row decoding, lazy first access, winner fetch, and DML scan decode are covered. Output copies and UPDATE's row copy are intentionally excluded by the documented metric. Query/distance counters preserve baseline behavior.
- src/storage/store.cpp:231 prechecks mutation count, combined value payload, and duplicate keys before appending. It sorts signed-order keys, permits a single oversized value, and handles missing deletes without append. Recursive merge at :142 groups shared prefixes and copies each changed ancestor once. Every new child precedes its parent; root changes only after complete merge success at :254.
- Existing FORMAT02 constants, encodings, checksums, old-page ordering, root savepoints, commit fsync calls and publication ordering remain unchanged. Batch writes remain compatible with the existing reader by construction. The implementer's separate old-reader binary roundtrip was not rerun in this review.
- src/sql/executor.cpp:15 bounds queued mutations to 128 and 512 KiB, except one oversized row. UPDATE/DELETE scan the captured immutable root while publishing new roots. Statement savepoint at :259 restores all prior chunks on failure and retains earlier statements in the transaction. Existing parse/bind input buffers are unchanged; no whole-database collection was added.

## Independent execution evidence

Created work/optimization-spec-probe.cpp and compiled against build/core/libminivectordb_core.a with g++ -std=c++20 -O2. Ran work/optimization-spec-probe against data/optimization-spec/differential-01.

Result: PASS independent four-mode differential: 262 rows, signed extremes, primary not first, scan/lookup/TopK/spill, counters, duplicate rollback, multi-chunk DML, commit/reopen.

The probe compared complete rows, labels, affected rows, examined rows and computed distances across all four Options combinations. It explicitly checked eager SELECT materialization 262 vectors versus selective 0 for a scalar projection/filter, and DML full scan counts of 262 vectors/1048 Cells in all modes. It exercised primary key in the third column, both int64 extremes, a sorter forced by the 4096-byte budget, alternate projected distances, transaction rollback and restart equality.

Also independently ran:

    MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/optimization-spec ctest --test-dir /mnt/d/Codex/MiniVectorDB/build/core --output-on-failure -R 'core_(selective|batches|sql_failure|sql_batch)$'

Observed results: core_sql_failure PASS 1.11s; core_sql_batch PASS 14.17s; core_selective PASS 4.46s; core_batches PASS 2.71s. Total 4/4 passed, 22.48s. This covers truncated/corrupt unused fields; raw failed-batch root preservation without restore; size/duplicate rejection before append; snapshot scan mutation; namespace preservation; late-chunk SQL failure; 300-row DML; and 300KB/600KB byte-boundary values. Tests were correctness checks, not performance measurements.

## Benchmark readiness review

bench/optimization.cpp and bench/run_optimization.py are suitable for the proposed local factorial comparison. The same binary, seeded exact float32 dataset, shuffled insertion order, SQL batches of 100, and identical autocommit boundaries are used in all modes. Independent ranking truth at optimization.cpp:49 compares complete ids/order/text and distances; reopen checks stored vectors, scalar values and text. API timers exclude SQL string generation and independent validation; they include the row sink. Two query warmups are separate. Both filter categories and actual materialization counts are retained. The runner alternates mode order, preserves each completed JSONL sample, and summarizes medians.

bench/decode.cpp separates in-memory full validation/materialization from storage, parser and TopK. It compares paired checksums and 0/1/10/50/100 percent matches; decoder-only timing must remain separately attributed.

No benchmark correctness blocker identified. Interpretation limits: the zero/zero mode is an instrumented eager/single-write reference in the same optimized binary, not an untouched baseline executable; file growth is logical extent, not physical IO; max RSS and local cgroup peak include benchmark buffers/cache and are not official resource measurements; the planned 2000-row/40-query experiment cannot establish official scale or score. Materialization counter reductions are not themselves elapsed-time evidence. These limits are consistent with the current method document.

Proceed to independent code-quality review and isolated timing runs. No additional source changes requested by this specification review.

## Follow-up: validated float32 block copy (8f1a679)

Verdict: PASS. Reviewed commit 8f1a679ff0c4d59e13949e228cc5f9f198698578, limited to src/storage/row_codec.cpp and tests/selective_tests.cpp. No blocking specification issue found.

The LazyRowView constructor still validates every stored float32 for finiteness before predicates. Vector materialization at row_codec.cpp:129 rechecks dimension and available payload, creates its own vector, and copies the already validated object representations only on native little-endian hosts. The payload pointer is correct even when the vector is not the first field: bytes_.size()-in.remaining() equals the recorded field offset plus the 4-byte dimension header. The remaining-byte bound ensures the memcpy cannot extend past the row. The other-endian branch retains explicit little-endian scalar reconstruction. This does not change disk format, ordering/distance arithmetic, callback lifetime, caching, or counter updates.

New tests check exact bits for positive/negative zero, positive/negative smallest and largest subnormals, minimum normal, largest finite, and adjacent-to-one values. They assert one cached vector materialization and constructor rejection of both infinities and NaN before any Cell materialization.

Independently reran core_codec and core_selective from the current built library/tests with MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/optimization-spec. Observed 2/2 PASS: codec 0.02s, selective 4.36s, total 4.40s. No benchmark timing was performed. Previous A/B results remain historical evidence; new timing results must identify this new revision.
