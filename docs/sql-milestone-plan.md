# SQL milestone implementation plan

Scope approved by the user on 2026-11-09: table catalog, row codec, basic SQL CRUD, exact vector queries, integrated transactions and tests. Work stays in D:/Codex/MiniVectorDB on 2026-competition-round2. No initial-round changes, push or formal evaluation.

1. Extend the immutable disk radix tree with table namespaces under one durable root. Preserve savepoints and atomic cross-table commits; scans capture their starting root so UPDATE/DELETE can stream.
2. Store a versioned binary catalog in namespace zero and rows in table-ID namespaces. Encode BIGINT, bounded UTF-8 VARCHAR and float32 VECTOR explicitly. Reject malformed lengths and invalid values before writes.
3. Parse and bind the supported SQL subset. Every mutating statement has a savepoint; autocommit uses the same durable publication path. Explicit transaction errors undo only that statement.
4. Execute primary-key lookups directly and scan other predicates. Filter before distance work, use bounded TopK, and stream projected output. General ORDER BY uses byte-budgeted runs and bounded merge fan-in under the data directory.
5. Add a local stdin/stdout CLI marked --protocol dev-tsv. This is a development adapter, not an invented official competition contract. Update the container to run it and test process restart/kill behavior.
6. Run library, CLI and isolated container tests; independently review specification and code quality. Save authentic evidence and research in the same local checkpoint as code and Dockerfile.

Disk format 02 introduces 128-bit namespace/key routing. Format 01 developer databases must be rejected explicitly rather than silently interpreted or overwritten. There is no migration requirement for test artifacts.

Supported target subset: CREATE TABLE; INSERT with optional column list and multiple VALUES; SELECT */columns/L2_DISTANCE with WHERE conjunctions, ORDER BY and LIMIT; constant UPDATE; DELETE; BEGIN/COMMIT/ROLLBACK. Exactly one BIGINT primary key; at most one VECTOR column. No NULL, JOIN, aggregates, primary-key updates or fake index implementation. Unsupported syntax must fail.

Tests cover binary corruption, Unicode and float32, namespace isolation, late statement failures, cross-table commit/rollback/reopen, transaction-visible vectors, ordering/ties/limits, forced disk sorting, and committed/uncommitted crash recovery. Existing core tests remain.

Performance research is hypothesis-driven: selective decoding can avoid constructing vectors for rejected rows; late projection can reduce temporary objects but add point reads. Do not claim IO savings from CPU changes, or infer competition scores from microbenchmarks. Immutable pages remain immutable across savepoints.