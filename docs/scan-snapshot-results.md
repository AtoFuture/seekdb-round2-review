# Bounded scan snapshots: results and correctness

Implemented on `d55b183429fff52e1c6a1c78b73acc55d16d205c`, retaining the latest paired-scan, page-cache and compact-page optimizations. This submission snapshot packages the locally verified changes, Dockerfile and learning materials together. The user authorized a GitHub push after verification; no formal evaluation has been submitted by the agent.

## Decision and implementation

The previous actual Docker benchmark spent 17.49 seconds on indexed queries versus 6.39 seconds without an index. Repeated index/base traversal and consistency checking were the largest demonstrated opportunity. We selected a bounded snapshot of validated raw rows, before attempting approximate pruning or another storage format.

Each Database owns at most one table/index snapshot. The default arena is 8 MiB, including 16-byte record headers; fixed object/allocator overhead is additional. First eligible indexed TopK query records probation without allocation. A second uninterrupted eligible scan builds the snapshot and publishes it only after full index/base consistency validation. Subsequent queries validate every cached row, evaluate their own filters and exact distances, and refetch winners. All rows are retained, including rows rejected by the building query's filter. No query answers are cached.

Every parsed non-SELECT statement invalidates before binding or execution, including failed writes, DDL and transaction operations. Exceptions cannot publish partial builds. Oversized snapshots release the arena and remain refused until invalidation; allocation failure falls back to streaming. Close/reopen never restores a snapshot. `Options.scan_snapshot_bytes=0` disables this optimization while retaining previous optimizations. Disk format, commit synchronization and recovery are unchanged. External modifications while a database is open remain unsupported, as with the existing page cache.

Two separately reproduced correctness discrepancies were fixed on this same latest tree: float32 maximum values that round to finite values now round-trip through vector output; all distance ordering uses the returned Euclidean double rather than its pre-square-root value. Equal returned distances use the prescribed subsequent keys, without epsilon grouping. The initial benchmark version retained rejection of `1e-400`. During upload, the remote branch was found to contain a newer parser repair; the final integrated version preserves that repair and accepts extreme underflow as signed zero (see upload integration below). These fixes are not proof of the cause of the platform's aggregate admission failure.

## Actual default Docker JSONL workload

10,000 rows, 128 dimensions, 256-character text, 1,000 timed queries per sample, TopK 10; half the queries filter at 10% selectivity. Three rounds alternate indexed/unindexed order. Ten warmups per sample; timing includes request serialization, Docker transport, SQL execution and response parsing. Data and oracle generation and answer checking are outside timing. Input SHA256 is unchanged: `9c7a1ae137870b946a9251c2c6549f2c17e723dad1da29898e4c2660713f5424`.

| Median | Previous d55b183 | Current | Change |
| --- | ---: | ---: | ---: |
| Indexed 1,000 queries | 17,491.502 ms | 4,491.645 ms | 74.32% less time; 3.89x throughput |
| Unindexed 1,000 queries | 6,390.626 ms | 6,298.350 ms | 1.44% less time |
| Indexed load | 1,763.080 ms | 1,715.918 ms | 2.68% less time |
| Index build | 62.685 ms | 84.448 ms | 34.72% more time |
| Indexed container peak through queries | 65,859,584 B | 72,126,464 B | +6,266,880 B (5.98 MiB) |
| Indexed final database file | 42,778,624 B | 42,778,624 B | unchanged |
| Indexed cumulative physical read/write bytes through queries | 43,196,416 B | 43,196,416 B | unchanged |

All 6,000 timed queries, 60 warmups, 24 extra ANN checks and six restarts passed. ANN still uses exhaustive exact search. Runtime image: `sha256:2ba59e6c3712e2e258a60babea479f4cbdda59c741830a5e34fe52de1b00cb74`.

Previous and current images were measured in separate phases, not alternated against each other. The following same-binary on/off experiments isolate the snapshot effect. Index creation does not use this cache; its measured regression is retained and has not been attributed to a specific cause. Physical reads were zero in the warm filesystem-cache environment; cumulative I/O includes loading, not just queries. Peak memory includes container filesystem cache. These are local measurements, not official E or rank scores.

## Same-binary controlled experiments

Three alternating rounds, snapshot budget 0 versus 8 MiB, paired scans enabled in both. Final runs contain 48 samples, 2,880 main queries, 48 first-query checks and 48 full reopen checks. Digests, distances, result counts and file sizes match.

| Scenario | Queries/sample | Off ms | On ms | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| Dense 10,000 x 128 | 200 | 2720.803 | 674.163 | 75.22% |
| Sparse 2,000 x 128 | 40 | 2994.227 | 39.960 | 98.67% |
| Wide 2,000 x 768 | 40 | 417.567 | 101.786 | 75.62% |
| 5,000-character text, over budget | 40 | 431.463 | 421.526 | 2.30% |
| 30,000 rows, over budget | 40 | 2359.811 | 2279.589 | 3.40% |
| Reads after mutation phase | 40 | 51.156 | 27.599 | 46.05% |
| Mutation before every query | 40 | 82.659 | 80.004 | 3.21% |
| Mutation before every query, large text | 40 | 471.547 | 465.003 | 1.39% |

The small changes in refused-cache and interleaved workloads are treated as approximately neutral, not a demonstrated optimization. Sparse is a particular core workload, not an overall speedup claim. Dense first-query median was 15.634 -> 15.119 ms; sparse first query regressed about 8.5%, and interleaved large-text first query about 5.4%. Core dense cgroup peak rose 75,960,320 -> 84,267,008 B; this includes the in-process benchmark/oracle and is not database-only memory.

The initial immediate-build design regressed mutation-before-every-query time by 41.95% and large-text interleaving by 44.91%. We retained those samples and changed admission to probation before the final runs above. Initial overflow queries regressed 7.46%. Initial interleaved file-size counters were sampled before interleaved updates, and churn CPU omitted those updates; final instrumentation fixes both. Initial results must not be used as post-update file-size or full churn-CPU evidence.

## Validation and evidence

- Final Docker build: **29/29** internal suites passed in 146.37 seconds. Initial full run found an old squared-distance tie expectation; the retained failure log documents it. Tests now assert returned-distance ordering, including forced external sorting and strict adjacent-distance comparisons.
- Official public suite: **20/20 cases, 95 SQL assertions**. Independent model: **273 checks, 150 mutations, two kill/restarts**. Default JSONL runtime checks include readonly container root, no network, persistence, EOF and shutdown rollback.
- Snapshot regression suite covers formats 02/03, eager/selective decoding, budgets 0/1/60/4096, probation, refused builds, changed vectors/filters, table/index switching, commit/rollback/failed-write/DDL invalidation, callback failure, stale index, empty tables and reopen.
- Actual Docker numeric probes verify maximum-value output/reinsertion and indexed/unindexed returned-distance ties; true overflow is still rejected.
- Larger-than-memory check: 20,000 rows, 81,920,000 logical text bytes and about 180 MiB database file under a **64 MiB container limit**; queries and kill/restart rollback passed, no OOM kill, observed swap usage zero. WSL cannot enforce the requested no-swap limit, so this does not establish formal resource admission.

Raw evidence, including unsuccessful initial experiments, is in [`bench/results/snapshot-20261110`](../bench/results/snapshot-20261110/manifest.json). Core reproduction uses `bench/run_snapshot.py` and `bench/paired.cpp`; actual transport reproduction uses `bench/run_jsonl_baseline.py` with `MVDB_BENCH_IMAGE` and `MVDB_ORACLE_BIN`. Workload and resource parameters are recorded in the JSON evidence. No global WSL settings or initial-round branches were changed.

Remaining limitations: hidden formal cases and detailed platform failure logs are unavailable; the public error-code map is empty. This does not establish formal admission. Snapshots still perform exact O(N*d) search; tables above the budget fall back to streaming, the flat index duplicates vector storage, and there is no space reclamation. Future work should profile large-table streaming and index construction separately before changing persistence or recall contracts.


## Upload integration: preserve concurrent remote fixes

Before upload, GitHub branch head was found to be `8b7054ad45b76e8055d393fcd3e3d13717a072e8`, two commits ahead of the original base. Its float32 conversion checks, private-C-locale underflow parsing, new literal tests and documentation were retained. Snapshot caching and returned-distance sorting remain enabled. The old test expecting `1e-400` rejection failed on this merge, while the remote literal suite passed; it was replaced with signed-zero assertions, with genuine overflow and malformed-number rejection still checked. The initial failing merge test output is archived.

Final integrated image: `sha256:446a86b12b846b8366ac07a55801cc508f382735d3a14a22aa83dcc50ddf437e`. Fresh checks passed **30/30 internal suites**, **20/20 official public cases**, **273 independent model checks with two kill/restarts**, and runtime smoke checks. A fresh three-round actual JSONL benchmark passed **6,000 timed queries, 60 warmups, 24 ANN checks and six restarts**; indexed query median **4956.946 ms**, unindexed **7302.234 ms**. Earlier tables remain the original isolated optimization measurements; these fresh numbers apply to the integrated upload version.

Integration evidence is under `bench/results/snapshot-20261110/upload-merge/`. No formal submission or global configuration change was made. The hidden formal failure remains unconfirmed.
