# Submission contract and scoring measurement repair

Goal: test the user's Prompt-interface hypothesis against the actual published commit, and make local performance evidence follow the published measurement contract more closely. User explicitly authorized interface/path fixes and score-first work; no extra approval for reversible project work. Everything stays on D:, no global changes, push or formal submission.

Evidence before changes: freshly retrieved Yuque content equals previous content byte-for-byte (content SHA2562863dd247a90948dd1103594bc34d44c753797826b6f28002aba070f7b61ab11). Default material path remains agent/conversation.json, free business schema, strict UTF8 object <=128KiB. Official root conversation.json is an empty placeholder, not a template. Published524133 has both material paths as identical regular blobs0d39f6eea2ef220f79a8f985eba7a489d750395d. Current path audit has not found a missing repository reference. Do not invent a filename bug or claim the opaque platform error is solved.

## 1. Submission preflight
- New scripts/check-submission-contract.py: standalone Python3.7+, explicit repository/path and optional immutable Git ref; verify actual file identity, regular file mode, exact case, UTF8/JSON/limit, optional root mirror and internal evidence references. No mandatory business fields or locally generated platform score/status.
- New tests/submission_contract_tests.py: synthetic missing-path, wrong-case, empty placeholder, encoding/JSON/size, free schema, mirror and historical-Git-vs-dirty checks. Run RED before implementation and GREEN after.
- Run against524133 and the current worktree. Archive actual GitHub blob identity too. A successful local check establishes packaging, not platform read success or evaluation validity.

## 2. Portable public tools and benchmark
- Vendor unchanged public f4d4e266 protocol_check/mysqltest/STDIO_PROTOCOL files with Git-blob identity lock and attribution. No official performance baseline exists in that102-entry public tree; generic SeekDB/PowerContext baselines are different tasks.
- Add a portable benchmark using those official transport files, explicit output/data/temp paths, actual image IDs and an independent full-sort C++ oracle. Keep previous benchmark/evidence unchanged.
- Default workload matches published10000rows/128dims/1000queries/K10/batch100/half-filtered. Generated distribution, seed, filter selection and warmup remain explicitly local choices, not frozen official ones.
- Keep correct-result validation out of timed query sequence: collect decoded replies on host, then validate after the resource window. Retain per-request protocol checks and correct final file/content/restart verification.
- Observe cgroup data-device I/O and container peak before and after named phases. After workload, keep business process/cgroup alive; fsync only owned database files/directories from host, then wait boundedly for dirty/writeback=0 and stable device counters. Record missing data as unavailable/error, never zero. Do not globally sync/drop caches or change swap.
- Retain original published per-request timeout. Overall performance duration remains a documented local limit, separate from the short public functional case timeout.
- Separate write/query resource sessions, no index in write benchmark; query preparation/index/warmup counted in B/M but excluded from query T. Three counterbalanced rounds compare pinned524133 and candidate image. Report actual T/B/M and recall, never synthesize platform E from unknown official baseline.

## 3. Acceptance and records
- Test the new harness helpers and run a small end-to-end smoke before the standard comparison. Do not rebuild unchanged database or rerun its41 tests solely for tooling/doc changes.
- Keep successful and unfavorable timing observations. Document remaining environment and official-service unknowns.
- Recheck material references, append only authentic current request/results if space permits, preserve old material/manifest snapshots before updating current submission binding. Existing runtime/source evidence remains frozen.
- Final report must answer which path was previously used, whether any path bug was found, what official baseline exists, exactly what was repaired, and what still needs platform-side evidence.

## 4. Cache-switch regression discovered by the measurement repair
- The first diagnostic VARCHAR run (10000 rows, 128 dimensions, 1000 TopK10 requests, half filtered) found a material regression: baseline/candidate query wall time 3118.860/12672.420ms unindexed and 3040.152/21053.550ms indexed; write time improved 1747.529/1370.463ms. All six sessions passed independent correctness and restart checks. These are one-round observations, not final three-round medians or official scores.
- The prior runtime comparison covered BIGINT predicates. VARCHAR-filtered and unfiltered requests alternate between raw-row and numeric cache eligibility; rebuilding or discarding cache state on each switch can defeat reuse. The working repair is to keep a valid raw snapshot stable until its existing invalidation boundary rather than repeatedly promoting it back to the numeric cache.
- First demonstrate the switching failure using counters and a regression test, then implement the smallest cache-state repair. Preserve full validation, transaction/write invalidation, the shared memory bound, and exact query semantics. Do not special-case fixture values or precompute answers.
- Validate both BIGINT and VARCHAR paths, indexed/unindexed, including real writes and restart. Freeze a new runtime image only after correctness checks, then run the two filter types through the new local comparison harness for three counterbalanced rounds. Preserve the failing diagnostic and report any regressions; no inferred official E.
- Final results belong in docs/submission-contract-results.md. The Prompt path audit is independent of this performance bug: finding a cache regression does not establish the cause of the platform's opaque Prompt-status message.
