# Independent SQL milestone specification review

Date: 2026-11-09
Verdict: YES, the reviewed implementation satisfies this milestone; no unresolved blocking findings.
Scope: baseline 78a15be0789f88dd201b7670a282380e26473f7a through library fix 7c140e88e167aaef6255f0e509ed980040f414f7, plus current CLI/tests/Docker/docs working tree. Branch verified: 2026-competition-round2. Read-only repository review; no reviewer source edits, commits, pushes, formal evaluation, or global configuration changes.

## Finding resolved during review

[P2] VARCHAR storage width incorrectly constrained WHERE comparison constants. At original src/sql/binder.cpp:81, predicate binding reused bind_literal, whose VARCHAR branch enforced the column extent. Reproducer: create p(id BIGINT PRIMARY KEY,s VARCHAR(1)); insert (1,'a'),(2,'b'); SELECT id FROM p WHERE s<'aa' returned ERROR TYPE instead of id 1. UPDATE and DELETE shared the defect.

Fix 7c140e8 separates predicate validation at src/sql/binder.cpp:47-56 and uses it at line 95. It preserves string type and UTF-8 validation without storage-width restriction. INSERT/SET still use the bounded validator. Independently reran the original reproducer: SELECT returned id 1, valid LIMIT 0 returned END 0, UPDATE and DELETE with longer constants succeeded. An oversized SET against a nonexistent id still rejected before execution, as required. Added regressions at tests/sql_tests.cpp:202 cover comparison operators, Unicode, LIMIT 0, DML, malformed UTF-8, and unchanged storage validation.

## Verification evidence

- Independently ran 8 focused CTests after the fix: core_namespaces, core_codec, core_sql, core_sort, core_sql_crash, core_sql_edges, core_sql_failure, cli. Result: 8/8 PASS, 8.71 seconds.
- Independently authored and ran work/sql-spec-probe.py against the final binary: 120 rows and 16 expected result sets PASS. Includes signed keys, Unicode multicolumn spill sorting at a 4096-byte working budget, filtered L2 TopK and large-K fallback, UPDATE/DELETE snapshot scans, rollback, and temporary-run cleanup.
- Inspected actual parser/binder/executor/sorter, binary row/catalog formats, shared-root Store, exact ranking, CLI framing, test sources, CMake, Dockerfile, and protocol/README boundaries.

## Requirement assessment

- Shared-root table namespaces and catalog counter/schema updates are atomic under savepoint and commit. Streaming mutations traverse immutable captured subtrees.
- Codec is explicit/versioned and checks dimensions, finite float32, UTF-8 scalar length, lengths/truncation, primary-key agreement; format 01 rejects explicitly.
- Required CRUD/projection/predicates/order/limit/transaction subset is implemented, validates before mutation or LIMIT 0, and rejects unsupported statement structures. Statement rollback preserves earlier explicit-transaction work.
- Vectors are read from current transaction rows. Filtering precedes distance computation; squared distances determine ranking and primary-key ascending ties, including rounded-distance near ties.
- Small K uses a bounded heap, general ordering spills to per-query directories with two-way merge and exception cleanup. No database-wide materialized row/result vector is introduced. README accurately discloses full-row decoding and sort budget overhead.
- CLI explicitly selects dev-tsv, frames semicolons/quotes/comments, flushes statement results, invalidates partial results on ERROR, and relies on Database destruction for EOF rollback. Existing CLI tests include acknowledged-commit SIGKILL recovery.
- Existing core suites retained; focused reviewer tests independently passed. Docker test/runtime definitions are present, but container execution is being verified by the parent and was not independently rerun here.

Official protocol compatibility, ANN requirements, formal evaluation, and performance targets are outside this milestone and are not claimed by this verdict. This is focused review evidence, not an exhaustive proof or repeat of the full slow core-storage suite.
