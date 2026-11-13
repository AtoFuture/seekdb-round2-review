# Bounded scan snapshot and numeric correctness plan

User authorizes autonomous design, self-review and implementation. Work in the existing isolated second-round D-drive checkout based on d55b183; do not push or submit. No initial-round or global setting changes.

## Design and alternatives

The indexed JSONL workload takes 17.49s versus 6.39s without index. Repeated paired traversal and repeated index/base comparison remain avoidable. Choose one bounded raw-row snapshot for eligible indexed TopK. Retain per-query row validation, filtering, exact distance calculation and winner refetch; reuse only the successful index/base consistency proof for owned immutable bytes. Alternative planner bypass of index saves work but does not improve the index path. ANN/compression changes recall/storage contracts and is deferred.

Cache is instance-local, one table/index identity, fixed arena <=8MiB including 16-byte per-record headers; fixed object/allocator overhead excluded. Following an observed +42% interleaved write/read regression, first eligible query now records probation without allocation; second uninterrupted eligible query appends every validated base row, including filtered-out rows, and publishes only after full equal-key scan succeeds. Any parsed non-SELECT execution invalidates before mutation/transaction handling; parse failures cannot change state and retain valid cache. Error aborts the build. Oversized tables are marked refused until invalidation, arena released, and streaming proceeds; no repeated failed builds. Zero budget disables. Empty table can be cached. No disk format changes, no cache persistence, no change to fsync/commit behavior. Reentrancy is already prohibited; external file modifications while open are unsupported. Memory allocation failure falls back to streaming.

Numeric fixes independently define ORDER BY distance using the same finite double Euclidean value returned by L2_DISTANCE, not its pre-sqrt value; preserve strict comparisons and primary-key ties (no epsilon grouping). Float32 conversion must accept finite rounded values near max and reject actual overflow. Round-trip own vector output must succeed. Underflow parsing contract requires explicit tests rather than silent broadening.

## Tasks and acceptance

- [x] Numeric worker: add failing regression cases, verify failure, fix float32 boundary and both heap/general-sort paths; review existing squared-distance-specific expectations. Own types/value.cpp, vector/exact.cpp, binder numeric helper and numeric tests; coordinate executor edits with parent.
- [x] Cache tests first: default/reference equality; first miss and later hit; queries with different vector/filter; rollback/commit/failing statement/DDL invalidation; budget overflow/tiny/disabled; failed build not published; both formats and selective decoding options; close/reopen and stale index detection. New dedicated CTest suite.
- [x] Cache implementation: private sql/scan_cache.hpp with bounded arena and explicit state; executor indexed path stores only fully validated rows, cached path validates rows afresh and counts cached rows separately; no Store API additions.
- [x] Extend paired benchmark with snapshot budget flag and cache counters; same binary on/off, alternating three rounds across dense/sparse/wide/overflow/overcache/churn. Preserve exact truth, file sizes and recovery comparisons. Benchmark truth uses returned Euclidean distances after numeric fix.
- [x] Retain default only after substantial typical-query improvement, inspect first-query and oversized-table regressions and memory cost. Otherwise leave disabled and report negative result.
- [x] Independent code review, all internal suites, official public suite, independent model/crashes, numeric probe, actual default Docker JSONL comparison. Archive honest results and learning evidence; no formal-score claim.

Self-review: cache bounded independent of dataset; conservative invalidation covers statement failures and transaction restoration; partial builders cannot become readable; tiny budgets safe; raw cache preserves every row validation and uses index proof only while database state is unchanged. No user clarification needed under explicit autonomous authorization.
