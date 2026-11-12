# Portfolio measurement method

The reference is bytewise IEEE CRC plus zero cache in the SAME executable, not a separately compiled historical binary. All arms share executable, image, data and resources. Six combinations run alternating/rotated order. Raw records preserve source/binary hashes. Two warm queries precede timing; counter reset retains cache contents. Diagnostics enable phase clocks separately; cache counters stay active.

SQL: 128 float32 dimensions, 256-byte text, shuffled rows, durable 100-row statements, half unfiltered and half 10%-selectivity Top10. Independent truth and reopen checks are outside timing. File size, digest and distance/materialization counts must match. Cache entry allocation includes metadata, excluding fixed bookkeeping and allocator overhead. Peak RSS/cgroup includes harness records and results, not engine-only memory.

Data /var/lib/minivectordb-portfolio is inside the D-backed MiniVectorDB Linux disk. Containers use 2 CPU/512 MiB, read-only root and no network. OS cache is warm/uncontrolled, swap-limit warning remains, and global settings are unchanged. Logical reads are not physical SSD IO. Local results are not official MySQL admission or ranking.

The prototype has no SQL, transactions, recovery, updates or migration. Its synthetic 12-byte payload differs from SQL; one-row pages are a layout control. Never multiply prototype and production speedups. Cluster IDs, group labels and filtered query clusters correlate: filtered queries search their own cluster. Uniform data is a separate control, and negative outcomes must remain visible. Query timing includes open/close and reference comparison. all_formats_build_ms is aggregate construction of all formats without fsync, not per-mode durable insert time.

File sizes/read counts include requested metadata, boundaries, summaries and candidates. Bounded page buffers + fixed bounds + O(k) describe query algorithm state only: generation/reference initially hold full rows, and harness queries/truth remain. Requested pages are CRC checked; skipped pages are not scrubbed. See experiments/README.md for floating-point assumptions and conservative bound proof.

Compact-page production integration requires immutable publication, overflow handling, atomic data/summary generations, migration and recovery tests. VA requires strict-bound exact pruning and safe full-scan fallback. Small differences between cache budgets are workload-specific.
