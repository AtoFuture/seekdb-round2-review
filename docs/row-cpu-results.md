# Ranked row CPU optimization results

Base: uploaded `cc0ae3f5418e8d56ebdb9530e83c96a85c8a36ba`. Changes remain local on `2026-competition-round2`; no push or formal submission. All builds, data, logs and artifacts use D: or the dedicated D-backed MiniVectorDB WSL disk.

## Why this order

The prior instrumented dense probe attributed 32.35% of query time to full row validation, 14.56% to materialization and 11.64% to distance calculation. The 41.45% remainder includes allocator work, callbacks, access, execution and instrumentation; it is not an allocation measurement. The write probe spent 65.47% synchronizing, but a durable COW commit needs both barriers. Larger internal batches cannot reduce commits in the existing 100-row-statement workload.

1. Batch full vector validation without changing floating-point distance arithmetic.
2. Remove two per-row metadata allocations for schemas with at most eight columns.
3. Next: consider a private validated-vector consumer to avoid repeated checks and allocation/copy, after preserving unaligned loads and exact returned-distance ordering.
4. Profile write numeric parsing/encoding and cached index-page copying before changing either; their isolated benefits are not yet measured.
5. Treat a one-sync WAL as a separate crash-recovery/format milestone. Do not remove a barrier from the current protocol. Larger caches and cold I/O need separate larger-than-cache evidence.

## Implementation and safety

The lazy row constructor still validates every field before predicates/projection. It checks vector length, takes exactly the bounded byte range, loads uint32 words with memcpy and reduces a per-component non-finite exponent test. It never ORs exponents across components before testing. Little/big-endian masks are explicit; mixed-endian platforms use the scalar reader. The private scalar reference remains available. No unaligned float-pointer casts, arithmetic reassociation, approximation or validation cache is introduced.

Each view owns eight inline offsets and optional Cell slots. Wider schemas retain the two-vector heap path. The mode is captured at construction, and no self-referencing pointer/span is stored, so ordinary copy/move destinations remain independent. Cells remain lazy and retain their existing counters. Cell references must not be retained across moving their view. Review restored the old heap `.at()` bounds behavior for a moved-from source, and guarded the benchmark's recovered-row index before access.

Before production edits, an independent serialized-row/oracle test passed 88,718 comparisons on the old core. The short-schema allocation requirement failed as expected with two allocations. The final suite passes 354,944 comparisons across scalar/batched and heap/inline modes, including guard pages, unaligned bytes, NaN/Inf, finite extremes, signed zero, subnormals, truncation, trailing data, widths 1/4/5/7/8/9/17/64, copy/move lifetimes and thread-local controls. Allocation checks now report zero metadata allocations for 1..8 BIGINT columns. Vector/string Cells may still allocate when materialized.

## Sequential ablations, including negatives

Stage 1, before adding inline metadata: dense 552.463 -> 540.497 ms (-2.17%); 768 dimensions 98.507 -> 90.728 ms (-7.90%). Interleaved query time rose 1.50% and churn query time rose 3.22%; both are retained.

Stage 2: dense batched/heap 568.465 -> batched/inline 461.145 ms (-18.88%). Across the three-mode control:

| Workload | Scalar/heap ms | Batched/inline ms | Query change |
| --- | ---: | ---: | ---: |
| dense | 590.073 | 461.145 | -21.85% |

Stage 2 negative controls are also retained: mixed Unicode +1.33%, 768 dimensions +3.39%, overflow text +8.64%, 64 columns +10.19%. At 9/64 columns both switches choose the heap path, so differences there are measurement variation, not a claimed inline benefit. These controls also share the new arrays/frame size and initialization; the forced-heap mode is not the unchanged uploaded binary. This prompted the direct old/new comparison below, rather than hiding unfavorable samples. Stage 2 evidence predates the defensive `.at()` review correction; final controls and Docker validation include it.

## Direct comparison with the actual uploaded core

The old core library and public headers were extracted from immutable uploaded-version test image `sha256:ec216e8814f82dc0dbce52f34639ceedbc123c909db4f992ac1c803ec6ce7e55`. Both libraries were linked to the same current benchmark harness; the old core ignores new private controls and retains its original scalar validation/heap metadata. Counterbalanced fresh-container runs use a two-CPU quota and 512 MiB limit, three rounds per mode. Profiling is disabled in this table.

Dense is 10,000 rows/128 dimensions/200 queries. Other cases are 2,000 rows/40 queries; wide uses 768 dimensions, small uses 16, overflow uses 5,000-character text. Eight/nine/64-column cases test the inline boundary and fallback.

| Workload | Uploaded core ms | Final core ms | Query change |
| --- | ---: | ---: | ---: |
| dense | 613.210 | 538.062 | -12.25% |
| unicode | 36.687 | 32.829 | -10.52% |
| wide | 99.253 | 88.871 | -10.46% |
| overflow | 392.925 | 376.915 | -4.07% |
| interleaved | 79.083 | 75.189 | -4.92% |
| small | 13.971 | 11.134 | -20.31% |
| sparse | 38.290 | 32.431 | -15.30% |
| churn | 22.460 | 21.511 | -4.22% |
| no_snapshot | 66.863 | 60.099 | -10.12% |
| eight_columns | 25.723 | 21.670 | -15.76% |
| nine_columns | 29.530 | 27.807 | -5.83% |
| wide_schema | 47.659 | 46.772 | -1.86% |

For interleaved updates, query time alone excludes UPDATE/commit. The sum of median query and mutation times is 440.469 -> 439.926 ms, approximately neutral; it is not the median of per-sample sums. Dense write time is 1399.926 -> 1391.147 ms; index construction is 60.389 -> 62.523 ms (+3.53%, retained). Dense core container peak is 84,729,856 -> 84,791,296 bytes. File sizes, exact mode-to-mode result digests, row/distance/materialization counts and reopened contents match. Main distances use a tight independent-oracle tolerance; first-query distance comparisons and mode-to-mode returned-bit digests are exact.

Single-round instrumented probes are diagnostic only: dense total 22.228 -> 17.858 ms, validation 7.626 -> 5.909 ms; wide total 108.800 -> 117.974 ms is a retained negative diagnostic, not substituted for the uninstrumented medians. Timers add substantial overhead. Memory peaks include generated data/results and charged file cache through the query phase, excluding later oracle sorting/reopen work.

Across all uninstrumented stages: **201 samples, 11,400 timed queries, 201 first-query checks and 201 complete reopens**. Separate instrumented probes add ten samples/300 queries. No test or benchmark ran concurrently with another CPU-intensive workload started by this task.

## Final Docker and real JSONL

Runtime: `sha256:6201d1c09043e83ff37a77a58b19f9615a5addfd4f6dd170a71e185fe54e77cd`; test image: `sha256:e32e12f9ce3121aa863ab4edd41d01458cc933cbc29a3afb1fcd73e77adadf89`. **33/33 internal tests passed in 168.88 seconds**, official public **20/20 (95 SQL assertions)**, independent **273 model checks/150 mutations/two kill-restarts**, and default JSONL/runtime/persistence/rollback smoke checks passed.

The actual old/new runtime images are also compared in alternating version order over three rounds, each running indexed and unindexed 10,000-row/128-dimensional/1,000-query workloads with an external oracle. These JSONL runs use a two-CPU quota and 2 GiB memory limit; core comparisons and the runtime smoke check use 512 MiB. Timing includes JSON encoding, Docker pipe transport, server work and response parsing. All **12,000 timed queries, 120 warmups, 48 extra ANN checks and 12 restarts** pass. Input hash remains `9c7a1ae137870b946a9251c2c6549f2c17e723dad1da29898e4c2660713f5424`.

| Runtime metric | Uploaded | Final | Change |
| --- | ---: | ---: | ---: |
| indexed / query_ms | 4179.395 | 3342.662 | -20.02% |
| indexed / write_ms | 1822.780 | 1810.851 | -0.65% |
| indexed / index_build_ms | 70.546 | 70.076 | -0.67% |
| indexed / peak_bytes | 72585216.000 | 72978432.000 | +0.54% |
| indexed / database_bytes | 42778624.000 | 42778624.000 | +0.00% |
| unindexed / query_ms | 6020.032 | 5256.714 | -12.68% |
| unindexed / write_ms | 1847.910 | 1837.936 | -0.54% |
| unindexed / peak_bytes | 53379072.000 | 53342208.000 | -0.07% |
| unindexed / database_bytes | 31379456.000 | 31379456.000 | +0.00% |

Peak and file metrics are bytes; other metrics are milliseconds. Indexed cumulative physical read/write bytes through the query phase are 43,196,416 in all six old/new samples, unchanged; this includes startup/load/index/warmup and is not query-only I/O. This is local warm-cache evidence, not the official frozen benchmark or a ranking score. WSL cannot enforce the no-swap limit. Public-case success does not establish hidden formal admission; the reported failed formal SHA remains the older 0e954 and its hidden failure category is unavailable.

## Reproduction and remaining priorities

`bench/run_row_cpu.py` supports reference/batched/inline ablations and an uploaded-core control. Compile `bench/paired.cpp` against the respective libraries; use distinct output paths. Private controls are only connected to the benchmark, not the production JSONL protocol. `bench/run_jsonl_versions.py` runs the actual runtime comparison using `bench/run_jsonl_baseline.py`. Raw measurements, test logs, image IDs and source/evidence hashes are under `bench/results/row-cpu-20261110/manifest.json`.

Next prioritize a safe private validated-vector path and measured numeric-write CPU work. The current distance loop, persistence barriers, disk format and memory-cache budgets remain unchanged. Learning records distinguish the user's prioritization request from agent decisions and actual tool measurements; negative outcomes are not removed or rewritten as user understanding.
