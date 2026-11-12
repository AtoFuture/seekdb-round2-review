# Score-first execution plan — 2026-11-11

Goal: improve measured end-to-end time, physical I/O and container peak memory while retaining formal MySQL admission. User reports commit 524133baf0a4ed947805236c02cce9cecffc0be3 passed admission with E=856.552980; Prompt invalid aggregate reason unknown. This is the release baseline, not a failed implementation.

Authorization: user explicitly requests continuous score-directed optimization; prior authorized design is validated typed data and reduced candidate work, plus commit-path experiments. Do not push or submit in this phase. All artifacts D:/Codex/MiniVectorDB; no global or initial-round changes.

## Evidence and alternatives
- Raw snapshot cuts traversal but repeats full-row validation and copy. Final diagnostic 60 queries ~221ms: validation72ms, distance32ms. Probe overhead means these are locating metrics, not production percentages.
- Write100batches has200fsync, ~842ms of1322ms. Initial concern: the original persistence ordering needs both barriers; any reduced-barrier experiment must state and prove its narrower failure model (reviewed decision below).
- Choose bounded typed read representation first (lower integration risk, exact answer remains). ANN first would not cure repeated row processing and introduces recall/filter risk. WAL has a large independent durability blast radius: prototype/verify before production integration.
- Query representation follows materialization/vector-processing ideas (https://ir.cwi.nl/pub/11098/11098B.pdf), not reuse of an external database engine.

## A. Bounded typed query snapshot
- Replace (never stack with) the raw snapshot when eligible TopK predicates use BIGINT columns. Keep raw streaming fallback for unsupported shapes, tiny budgets, oversized tables, primary lookup and ordinary SQL.
- Snapshot owns aligned float vectors and int64 key/scalar columns, with total numeric buffers at most min(scan_snapshot_bytes,8MiB), metadata bounded by schema size. At most one ready snapshot per executor. Default probation: first eligible read scans only; second builds; subsequent reads reuse.
- Full base-row/schema/vector validation and index/base consistency occur during complete construction. All rows are included even if builder query filters them out. Publish only after full scan succeeds. Immutable owned representation reuses that proof: no revalidation/copy of unrelated VARCHAR fields on replay. Disk content is not assumed validated on initial load. External concurrent disk mutation is outside official crash model.
- Invalidate before every parsed non-SELECT including failing DML/DDL, BEGIN/COMMIT/ROLLBACK; preserve statement failure semantics and rollback. Schema/table/index/vector identity binds cache. Switching incompatible shape releases typed cache before raw-cache allocation. Reject partial/overflow build and fall back.
- Keep exact float64 accumulation, rounded L2 ordering and signed primary-key tie break unchanged. Winner projection follows existing fetch. New diagnostic counters must distinguish typed replay from raw replay; do not relabel eliminated validation as executed.
- Separate option enables same-binary reference. Default enabled only after counterfactual tests and regression/bench gates.

## Tasks
- [x] Independent integration test first: warm complete snapshot then compare exact answer and require validation bounded by winners, cold/failed construction still validates all rows. Record actual baseline failure.
- [x] Implement typed cache and executor integration; test budget, identity and overflow in private cache unit tests.
- [x] Differential tests: indexed/unindexed, changing BIGINT predicates including zero matches, negative/sparse keys, ties/extreme vectors, >8 columns, VARCHAR fallback, multi-table, write/rollback/DDL/reopen, failed mutation and sink exception.
- [x] Same-binary off/on experiments: dense10k x128/200query (actual JSONL separately uses1000query), cold/second/warm, wide, overflow, mutation churn; time/resource metrics independent of correctness oracle. Retain regressions.
- [x] Compare actual default JSONL Docker against pinned baseline image91fe268d; alternating indexed/unindexed three rounds; verify exact output and restart, record write/build/query plus cgroup I/O/peak.
- [x] Full container suite and public protocol/model/runtime. No acceptance based on local E guesses.

## B. Write path (independent experiment)
- [x] Review WAL versus explicit COW commit policies. Decision: defer full WAL; implement and verify ProcessCrash with finalfsync-beforeACK, preserve Ordered. This supersedes the initial append-log prototype, which was not implemented in this round.
- [x] Benchmark same100-row batches with crash/fault counterexamples. Integrate only if both safety and measured total benefit justify; otherwise document precise next step.

## C. Score / Prompt contract
- [x] Recheck latest official upstream and accessible text; record discrepancy: cached f(R)=R/(1+R) bounds E<1 while actual leaderboard exceeds5000; cached Prompt unbounded while actual aggregate validator says0–100.
- [x] Verify material from exact submitted commit and prepare concrete platform-review questions without contacting anyone. Do not invent evaluation summary fields or truncate/self-score.
- [x] Preserve user/Agent provenance and append actual evidence only. Local hash manifest is not platform evaluator summary.

## Completion gate
Document measured wins and costs, correctly label official vs local results, preserve baseline, source+Dockerfile+material stay compatible. If lower-risk query gains pass, keep them even while larger write protocol remains experimental. Reassess dominant residual costs using new data before further changes.
## Reviewed scope decisions and subsequent hotspot (2026-11-11)
- The independent commit-policy review confirmed a smaller alternative to full WAL for the specified process-only crash model: all successful COW pwrite calls precede root publication; one final fsync of that same fd precedes ACK. It is an explicit ProcessCrash policy, with Ordered retained for data-before-root persistence ordering. File creation/directory sync/error poisoning stay unchanged. Actual media writeback failure/power loss is outside this proof. Baseline behavior RED (expected1 got2), 441 initial boundary cases GREEN, then add actual packed-page and same-fd coverage before final acceptance.
- Typed cache controlled experiments: dense sixround query429.470->128.720ms, memory72.64->69.52MB; actual JSONL validation still pending. FourMiB cache sixround standard workload lowerpeak and no slowdown, varied includes+~1ms overflow and roughlyneutral complete mutationcycle; no universal speedup claim.
- After typedcache, diagnostic full-rowvalidation fell to0.083ms per60queries; distance remains31.7ms. Next controlled microchange uses a private validated-candidate exactTopK entry: query checked once, each candidate dimension checked, SQL proof guarantees finite candidate components before sourcevisit. Existing public distance/TopK invalidcandidate checks unchanged. Same double accumulation order, sqrt-before-ranking and tie-breaking; no fast-math.
- [x] Privatekernel bitwise/oracle tests (random finitebitpatterns/extremes/subnormal/roundedties/multidim/K/predicates); record interface RED then implementation GREEN.
- [x] Same-binary privatekernel off/on measurements before enabling; retain publicreference and user-facing semantic gates.
- User further clarified exact objective: "对批量写入和向量 TopK 查询测量耗时 T、磁盘 I/O B、峰值内存 M优化的是这些". Compare all three, not just query speed. No invented local E calculation or Prompt score.
## Final local acceptance
41/41 Docker tests, public20/20, model273/2kills, default runtime,12000actualJSONLqueries/12restarts and64MiB resource check passed. Image source/binary identity verified. Query2.38x unindexed /2.30x indexed, write~22% lower time, cumulative querypeak~20-26% lower, blockI/O medians unchanged. See docs/score-first-results.md. Prompt append/check and manifest now passed: 122152bytes,108references,93current-source/167evidence hashes. No official new result or push.
