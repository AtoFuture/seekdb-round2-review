# Path profiling specification review

PASS: No blocking specification issue found by read-only inspection on 2026-11-09.

Reviewed baseline 07e16cf8e185495a01234f26ddc48b5579b4fc67 to instrumentation core 73bc598585d2243d52743b6b1de4336c3d35bd49, current bench/profile.cpp, bench/run_path_profile.py and docs/path-profile-plan.md; followed D:/Codex/MiniVectorDB/AGENTS.md. No builds/tests/workloads/source edits/commits/push performed. Parent reports final container tests 17/17 passed in 65.68 seconds; not independently rerun here.

Confirmed:
- Disabled-by-default thread-local enable/reset/snapshot API; reset preserves enable flag, disable preserves counters, disabled scopes skip clock calls.
- Constructor/destructor preserve errno; syscall counters cover every pread/pwrite/fsync attempt, including failures/retries; bytes increment only on positive transfer. Original retry behavior retained.
- Current instrumented checksum, eager decode, lazy validation, uncached materialization and squared-distance call graph has non-overlapping leaf scopes. Eager decode is explicitly a combined stage.
- Branch/overflow counts reflect typed requests, including failures, without added reads; no page cache or batch-read optimization introduced.
- Tests/CMake cover default-disabled behavior, reset/disable, thread isolation, operation results, failure decode counts and errno preservation. No dedicated short-I/O fault-injection test was added; direct inspection finds semantics unchanged.
- Benchmark prepares SQL before measured API calls; independent truth calculation and reopen checks are outside phase CPU/wall timing and snapshots. Warm-up is identical. Profile is disabled before reopen. JSON emission is structurally valid by inspection.
- Runner has exactly 12 cases: three rounds x two paths x on/off, alternating/reversing order. Same pinned image and binary, pre/post binary hashes, source hash, filesystem names, deterministic 2000 x 128 data / 40 queries, two CPU and 512 MiB settings. All cases verify truth/reopen, digests and invariant workload/file-size counters.
- Cleanup is restricted to owned containers and unique project-root temporary directories. No global cache flush/configuration change. Linux path relies on already-verified MiniVectorDB registry BasePath D:/Codex/MiniVectorDB/wsl.

Interpretation limits (non-blocking): leaf durations are elapsed times, not CPU attribution, and must not be added to enclosing phase durations. Phase CPU includes tiny measurement overhead; query API timing includes result callback collection. Use profile-off elapsed figures for performance, profile-on for breakdown, disclose on/off overhead. Two-query warm-up with no global cache flush supports warm path comparison only. Typed requests and syscall time cannot establish unique-page reuse, physical I/O or cache hit rates. Subsequent cache versus batch-read decision requires measured evidence. No performance results were inferred in this review.
