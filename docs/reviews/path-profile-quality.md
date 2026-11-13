# Path profiling quality review

PASS: No blocking correctness, profiling-boundary, or unsafe-cleanup defect found by bounded read-only source inspection on 2026-11-09.

Scope: stated instrumentation core 73bc598585d2243d52743b6b1de4336c3d35bd49 and baseline 07e16cf8e185495a01234f26ddc48b5579b4fc67. Inspected current profile.hpp, storage syscall/checksum/row instrumentation, both distance implementations, bench/profile.cpp, bench/run_path_profile.py and specification report. Followed project AGENTS.md. No builds, tests, workloads, source edits, commits, pushes or global changes performed. This report is the sole written artifact. Commit identities and runtime results are parent-supplied, not independently reexecuted.

Source findings:
- Thread-local state defaults to disabled/zero. Disabled scopes skip clock reads. Reset preserves enable state; disable preserves counters.
- Clock reads preserve errno. Syscall counters include failed attempts and retries; bytes count only positive transfers. EINTR/error handling retains syscall errno after scope destruction.
- Current synchronous timed stages do not overlap each other. Eager decode intentionally combines validation/materialization. Lazy materialization counts uncached cells. Lazy validation starts after constructor member initialization, so metrics do not exhaustively account for row setup/allocation costs.
- Database API phase timing excludes SQL string construction, exact truth checking and reopen validation. Query timing includes result callback collection. CPU timing includes tiny surrounding timing overhead. Identical warm-up is excluded from query snapshots.
- Twelve cases use the same pinned image/binary/data/options/limits: three rounds x two storage paths x profiling on/off, with alternating/reversed order, binary hashes before/after and correctness/digest/file-growth invariants.
- Cleanup uses only newly created container IDs and unique TemporaryDirectory-owned paths below dedicated benchmark roots. No broad deletion/cache flush/global configuration changes. Linux storage location relies on the verified MiniVectorDB WSL BasePath on D drive, documented as a runner precondition.

Parent-reported verification: container tests 17/17 passed in 65.68 seconds; 100-row on/off smoke results matched; final measurement completed 12 samples, 480 independently validated queries and 12 reopen checks. No verification was rerun during performance measurement.

Interpretation and next-step assessment:
- Leaf durations are elapsed times, not CPU attribution or physical I/O. Do not add them to enclosing phase times. They include collection costs and omit other execution work.
- Use profiling-off elapsed results for performance, profiling-on for breakdown, and disclose on/off differences. Same-binary on/off measures active collection overhead, not total instrumentation overhead versus the pre-instrumentation baseline.
- Identical two-query warm-up without cache flushing supports this small warm workload only; three rounds do not establish broader scaling or statistical confidence. Typed requests do not establish unique-page reuse, hit rate or physical reads.
- Supplied profiling-off query medians: Windows path 12,790.22 ms; native Linux path 697.98 ms. Native profiling-on query 709.23 ms includes pread 81.31 ms and checksum 569.70 ms. Windows profiling-on pread is 11,946.8 ms of 12,797.2 ms. These support storage-path-dependent syscall cost and a checksum-heavy native profile within the stated limitations.
- A bounded validated index-page-cache experiment is defensible as a WRITE-path hypothesis: branch requests are 31,965/32,005 typed write-phase reads. Request counts alone do not establish repeated page identities, attainable cache hits or capacity requirements.
- Query branch requests are only 7,960/88,440 typed reads (9.0%). Native checksum dominance includes overflow pages, so it does not specifically justify large query gains from an INDEX-only cache. The request fraction is not a strict latency bound because per-request costs differ. Quantify unique/repeated branch pages and measure query/write gains separately before adopting the experiment.
- Present evidence does not prove index caching superior to batch reads. No such claim is approved by this review.
