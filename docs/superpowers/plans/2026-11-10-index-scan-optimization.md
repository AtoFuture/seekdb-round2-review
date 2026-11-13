# Indexed TopK scan optimization plan

Goal: measure uploaded 0e954ffd90c6802ecd9fae4c0ca00eb8553201dd first, identify the dominant new weakness, review a bounded exact optimization, then implement only if experiments support it. User authorized plan/review/execution; all work stays on D, no push/submission/global settings changes.

Baseline gate: actual unchanged JSONL runtime df51f763, 10000 rows x128 dimensions,100-row autocommit batches,1000 TopK10 queries half filtered,3 rounds. Compare indexed/unindexed paths. Driver/truth outside database container. Save T and filesystem bytes; cgroup B/M only when measurable, clearly qualify cold-cache and swap constraints. No E/score without official reference baseline.

Weakness hypothesis: indexed TopK walks index then performs one Store.get_in per base row, rereading16 radix levels for every record even though both namespaces share identical ordered keys. Full row validation and vector equality are required; removing them is not this optimization.

Candidate A (preferred if confirmed): add Store.scan_equal_in(left_space,right_space,visitor) that snapshots one root, locates both namespace subtrees, and recursively traverses corresponding key prefixes together. Read each branch once per namespace; reject differing key sets as corruption. At leaves pass both values, retaining existing page checks, full row validation, vector equality, filters and exact heap. One shared bounded ScanCache, O(tree-height) traversal state and current value sizes. No file-format/commit changes. SQL option paired_index_scan permits same-binary reference path for comparison; production default only selected after evidence.

Candidate B: cache decoded complete rows/vectors. Potential lower CPU but duplicates memory, requires transaction-version keys/invalidation; do not adopt ahead of simpler traversal change.

Candidate C: HNSW/IVF or compressed VA pruning. Larger algorithm/recall/resource risks; defer until protocol-correct baseline and exact bottleneck removal measured.

Primary literature context: Boncz/Zukowski/Nes, MonetDB/X100 (CIDR2005), https://ir.cwi.nl/pub/16497/ studies execution/interpretation overhead and bounded vector processing. Our paired radix traversal is a project-specific inference about removing repeated navigation, not an implementation or claimed reproduction of X100's speedups.

- [x] Measure baseline and retain independent expected results; report requested current-version metrics early.
- [x] Independently review feasibility: root snapshot, mutation callbacks, missing keys, empty tables, signed key order, memory and cache bounds.
- [x] Add failing Store tests for equal/mismatched keys, snapshots during mutations, extremes, formats02/03, rollback; implement paired traversal.
- [x] Integrate indexed TopK with reference switch; tests compare exact/ann and assert reduced branch reads while preserving row-validation counts; catch missing/stale index rows.
- [x] Run same-binary interleaved comparisons, including larger-than-cache/wide/filtered/update workloads; retain regressions. Use actual Docker JSONL for end-to-end confirmation.
- [x] Run official public20, full internal tests, runtime and independent crash/model checks; spec and quality review.
- [x] Record complete evidence, truthful learning material and remaining weaknesses; no push or formal submission.

Acceptance: identical ordered IDs/distances, exact semantics, no additional unbounded state or weakened commit protocol; measurable indexed query improvement with unchanged data file layout and bounded resource usage. If no material gain, keep default old path and report negative result rather than promote an unproven optimization.
