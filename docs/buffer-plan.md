# Bounded cross-query data-page buffer: design and execution plan

Baseline bdae610c073a215d1fe85727ef85a2a8062ea238 on isolated2026-competition-round2; uploaded baseline remains unchanged remotely. User authorizes bottleneck identification, plan and execution. All writes/builds/logs/data D:/Codex/MiniVectorDB. No push, formal submission, initial-branch or global-setting changes.

## Diagnosis and alternatives

Current small profile spends20.27ms in pread and33.67ms in CRC out of129.93ms; distance4.58ms. Per-scan packed memo is rebuilt each scan and overflow data never cached. This makes validated data reuse the largest directly evidenced low-risk query opportunity, not a claim about unknown official workload or global optimum. Capture large profile before implementation. WAL writes offer separate gains but require a production recovery/index/checkpoint architecture; SIMD/distance has low current headroom. VA page skipping still violates full-row validation.

## Contract

Add trailing Store constructor data_cache_bytes=0 and SQL Options data_cache_bytes=0 for initial experiments. Expose separate data_cache_stats/reset_data_cache_stats on Store and Database using existing CacheStats; existing index statistics unchanged. Each Store lazily allocates a fixed bounded set-associative array (four ways, deterministic rank replacement), inclusive of page/entry/set metadata. Budget too small for one set disables cache. No per-page allocation, global cache or unbounded map/list. Fixed object/allocator overhead excluded and documented.

Cache only fully read, CRC/header/structure-validated packed and overflow pages. No root or index pages, no write admission, no disk format or commit changes. On every lookup enforce physical-address range and expected page type; cached immutable contents retain validation of all slot bounds/overflow links. SQL still checks every row and every UTF-8/finite-vector field on every query. Successful page IDs are append-only through rollback/savepoints, and partial failed writes never become reachable or cached. Closing a Store destroys its cache; external writes to an open file remain unsupported, close/reopen revalidates. Returned data is owned before any callback; nested scans/mutations remain safe.

Keep existing scan memo independently selectable; compare both layers and select defaults only after measurements. Preserve format02/03, exact answers, file bytes and two-sync durability. No claim of bounded total process RSS from the cache budget.

## Execution tasks

- [x] Inspect current profile, code invariants, branch and constraints; present alternatives and recommended design.
- [x] Independent read-only feasibility review, plus large baseline profile.
- [x] Storage implementer owns include/minivectordb/store.hpp, src/storage/store.cpp, tests/data_cache_tests.cpp. First capture a failing warm-repeat read assertion/API compile, then implement bounded cache and counters. Tests: budgets0/subset/large, overflow/packed, repeated scan/get no new pread on fitting set, conflict eviction, map equality, savepoints/rollback/callback mutation/nested scan, cross-Store isolation, close/reopen corruption with CRC-correct invalid pages and wrong-type cached access, real partial-write retry, SIGKILL reopen. No commits from worker.
- [x] Parent integrates Options/executor forwarding, CMake target core_data_cache timeout180, SQL regression combinations with cache on/off, full-row malformed rejection after page cache hit. Preserve existing SQL failure/crash suites under final defaults.
- [x] Independent spec then quality review; address findings before timing. Targeted tests then full suite.
- [x] Parent creates bench/buffer.cpp from frozen compact.cpp, adding data-cache controls/stats and separately timed first query with independent answer validation. First-query means application-cache cold for data (OS cache may be warm), not cold disk. Same-binary baseline packed=true/scan1MiB/data0; candidate data1/4/16MiB with scan0 and16MiB+scan1MiB. Keep one scan0/data0 arm if needed to separate per-scan from cross-query benefit. Fixed Docker2CPU512MiB, native Linux storage inside D-backed WSL,3 alternating rounds, no concurrent timed loads.
- [x] Workloads2000rows/40queries and10000rows/200queries; varied short/wide/768dim/filter/churn; larger than cache30000rows/40queries (estimate live packed data30MiB>16MiB), and one-shot/cold query metrics. All query digests and complete reopened key/value maps must agree; file sizes must remain identical across cache arms. Record misses, hits, evictions, allocated bytes and RSS. Choose a bounded default only with measured benefit; preserve regressions.
- [x] Fresh final Docker full suite and runtime restart, source and artifact hashes, authentic learning JSON mirrors (<128KiB), report and local commit. Update external checkpoint without claiming official admission or automatic upload.

## Research rationale

LeanStore (ICDE2018, author research page https://www.cs.cit.tum.de/dis/research/leanstore/) demonstrates the value of low-overhead buffer management for data exceeding memory. 2Q (VLDB1994 https://www.vldb.org/conf/1994/P439.PDF) motivates scan-pollution testing; this implementation is a small fixed-set cache, not an implementation of LeanStore or2Q. No third-party database engine is reused, and their performance numbers are not projected onto this task.

Implementation checkpoint: initial Store/SQL API compile-red captured, standalone storage green, integrated Release23/23 PASS115.44s. Five-arm smoke20 main queries+5 first-query checks+5 full reopens PASS; source labelled working tree, not used for speed claims. Large baseline confirms read+CRC58.39% versusdistance3.38%. SPEC reviewPASS; QUALITY PASS; proceed to archived performance runs.

Final decision: SQL16MiB data cache/scan memo0; low-level Store unchanged. Standard repeated-query improvements61.79%/61.35%;30000 rows beyond capacity35.94%. Retain first-query and write regressions plus memory tradeoff. Final Docker23/23 PASS123.87s and runtime persistence/EOF rollback PASS. Documentation, raw results and authentic learning are archived in the final local commit; no push.
