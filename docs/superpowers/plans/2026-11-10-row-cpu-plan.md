# Ranked performance work, based on cc0ae3f

User instruction: rank remaining problems by potential improvement, then optimize sequentially. All work stays on D:, on the existing second-round branch; no upload, formal submission or global configuration changes.

## Ranking and decision gates

The instrumented 2,000-row / 128-dimensional / 20-query probe attributes 32.35% to full row validation, 14.56% to materialization, 11.64% to distance calculation and 41.45% to other work plus instrumentation. This is not an official workload or a precise attribution of the remainder. Metadata allocation/destruction is partly outside those timers.

1. **Full vector validation in bounded batches.** Replace repeated reader bounds checks and per-component error branches with one checked byte range and an integer finite-exponent reduction that compilers can vectorize. Keep an independent scalar reference. Maximum possible gain from eliminating the entire measured validation stage would be 1.48x, and this change only addresses part of that stage; no numeric speedup promised in advance. Preserve every field check, including filtered and unprojected values.
2. **Bounded inline row metadata.** Avoid two heap allocations per short-schema LazyRowView using at most eight inline offsets/cell slots, with the original heap path for wider schemas and as a diagnostic reference. This targets allocator overhead in validation and the unseparated remainder. Per-view storage must remain bounded; copy/move, lazy materialization and independent views must remain correct. Gate default enablement on an additional controlled comparison after item 1.
3. **Validated vector distance/zero-copy consumption.** At most 11.64% measured distance time plus some materialization is directly addressable. Defer until the first two changes are measured: removing duplicate finite checks needs a strong validated-input boundary, unaligned row bytes cannot simply become float spans, and returned-double Euclidean ordering must remain exact.
4. **Durable write protocol.** Synchronization is 65.47% of the write probe, but load is only part of total benchmark time. Current two-sync COW commit cannot safely lose a sync. A one-sync WAL design needs replay, checkpoint ordering, truncation, crash injection and disk-growth accounting; it is a separate format/recovery milestone, not a local toggle.
5. **Snapshot capacity and cold I/O.** Existing warm-cache physical reads are zero and oversized snapshots already fall back. Raising cache limits consumes scored memory; evaluate only with larger-than-cache/cold data after CPU work, without claiming warm-cache wins apply to cold I/O.

## Implementation and verification

- [x] Independent row-codec oracle/bounds tests pass on the unchanged baseline.
- [x] Item 1 implemented with scalar reference; targeted tests pass in both modes.
- [x] Counterbalanced scalar/batched trials completed across dense, wide, Unicode, overflow and update-interleaved workloads; mixed and negative results retained.
- [x] Item 2 allocation requirement fails on baseline then passes; metadata lifetime/wide-schema tests and references pass.
- [x] Compare item 1 alone against items 1+2, retain only justified defaults.
- [x] Independently review implementation and test gaps; fix findings.
- [x] Final Docker internal/public/model/crash/runtime checks and actual JSONL performance run.
- [x] Archive raw evidence, hashes, limitations and authentic learning records; report results without claiming formal admission or score.

No floating-point reassociation, approximate search, disk-format change, skipped integrity validation or weakened commit durability is part of items 1 and 2.

Final evidence and measured qualifications: [row CPU results](../../row-cpu-results.md).
