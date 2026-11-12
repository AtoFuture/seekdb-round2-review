# Decode and write-amplification optimization plan

Goal: implement and measure selective row decoding and shared immutable radix paths without weakening SQL, transaction, validation or persistence semantics. User authorized this next phase; all artifacts remain on D, initial-round branches untouched, no push/formal submission.

Baseline: 7f3c4ac624ac952ffc524d925fcfee24bec1e4ae.

Official-package check on 2026-11-09:
- oceanbase/seekdb round2 still c842a368c3394748d0499386c18f6b465242e141, tree a7b13750a0baaa7ecbe6c270ad91014de94b969b.
- Recursive tree remains 50 entries, with empty Dockerfile/conversation.json and legacy configuration; no new official stdin/stdout driver or public test/result package.
- juanzhi-gcb/seekdb round2 remote returns Branch not found; expected because local work has never been pushed.
- Latest upstream release assets are normal seekdb packages, not a MiniVectorDB competition kit. The separate powercontext repository contains its application source and tests, not evidence of a seekdb round2 protocol.
- Continue using explicitly local dev-tsv; no invented official compatibility.

Architecture:
1. A lazy, per-row accessor validates complete encoded structure, text and finite vectors, then materializes only required cells. Predicate fields before vector/projection fields; never retain views beyond storage callbacks. Keep an eager reference path and real materialization counters.
2. Bounded batches of upserts/deletes share copied radix ancestors. Preserve all old pages, format02, savepoints and sync/publication order. At most 128 mutations and a 512 KiB SQL chunk target plus one oversized row. Any batch failure leaves its original root; statement savepoints roll back earlier chunks.
3. Keep independent Options switches so the same binary can compare baseline, decode-only, batch-only and combined modes. Do not infer physical IO from logical file growth.

Alternatives considered: mutable uncommitted pages risk saved-root corruption; replacing the tree with a B+tree increases format/recovery scope. Immutable batch construction is the smallest compatible change. SIMD/checksum/ANN and compaction are separate experiments.

Tasks:
- [x] Confirm branch/isolation and inspect upstream package state.
- [x] Add lazy accessor and eager/selective equivalence/error/counter tests; observe RED then GREEN.
- [x] Add immutable batch tree merge and SQL chunks; test random/signed keys, deletion, savepoints, failures and reopen; deterministic file-growth reduction.
- [x] Independently review specification then code quality and fix verified findings.
- [x] Build a reproducible four-mode benchmark, run a small correctness smoke, then fixed seeded datasets with alternating order and three rounds.
- [x] Compare write time and file bytes, query time by filtering group, materialized vectors, exact-result equality, and available memory diagnostics. Use explicit local scope; no official E/rank estimates.
- [x] Run current container/runtime regression tests. Save raw measurements, analysis, authentic learning evidence and local checkpoint.

Benchmark policy:
Use the same data, SQL batches of 100 and commit boundaries, stored float32 inputs and exact truth. Preserve all measured attempts with configuration/source identity. Pilot first to select a practical workload; do not silently change sample sizes mid-experiment. Warm-up, validation and timed regions are separate. No concurrent project tests/builds during measured rounds. A negative/noisy result is reported honestly; defaults follow measured representative behavior. Full official-size benchmark is not claimed unless actually run.

Correctness invariants:
- UTF-8/finite-vector validation is not skipped for filtered or unprojected data.
- All snapshots remain immutable. No fsync is removed and no autocommits coalesced.
- UPDATE/DELETE scan a captured root while batches publish a new working root.
- Disk format02 remains bidirectionally readable with the previous reader.
- Query results/order/errors must match independent truth and baseline across all switch combinations.
Completed: final library 8f1a679; v1 regression retained, v2 full four-mode and decoder reruns passed. See optimization-results.md and validation.md. No push/formal evaluation.
