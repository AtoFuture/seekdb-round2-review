# Optimization experiment method

These are local engineering A/B experiments, not official benchmark scores. The official protocol and frozen resource bundle remain unavailable.

## Factorial SQL experiment

bench/optimization.cpp uses the same binary with Options.selective_decode and Options.batch_writes independently enabled. The four configurations isolate each change and their combination.

- Seed 20261109; 128 float32 components whose binary values are exact multiples of 1/128.
- Shuffled primary-key insertion order; one scalar filter group (id modulo 10) and a 256-character payload.
- INSERT requests contain 100 rows and each request autocommits. No sync/commit is omitted or combined.
- Half the Top10 queries have no WHERE; half filter one group (10% selectivity).
- SQL strings are prepared outside each API timing interval. Timings include parse/bind/execute, persistence for writes, and the query result sink, but exclude stdin/stdout transport.
- Two warmup queries precede measurement. The reference evaluator and restart/full-row checker run after timing; all variants must agree with exact squared-distance ranking and persisted rows.
- File growth means append-file extent, including all index/record pages. It is not physical device IO.
- Separate containers use read-only root, no network, 2 CPU and 512 MiB. /data binds into the D-drive project. The benchmark binary mounts read-only.
- Per-process max RSS and local cgroup memory.peak are diagnostic only: they include workload generation/result buffers, and this WSL/bind-mount setup does not reproduce official swap/cache/writeback attribution.

bench/run_optimization.py runs three rounds, alternating configuration order. It refuses to overwrite old result files and saves each completed run immediately as JSONL. The summary uses medians, not fastest samples.

The 100-row/4-query smoke at selective-only commit verifies the harness, not performance; the batch option was not implemented then. Those numbers are explicitly excluded from optimization conclusions.

For the main local experiment, use 2000 rows and 40 queries per configuration, three rounds. This reduced workload keeps the four-mode comparison practical on the Windows-mounted data filesystem. It is not the official 10000-row/1000-query workload.

## Decoder-only experiment

bench/decode.cpp separately measures complete binary validation, materialization and filtered squared-distance accumulation in memory. It uses 10000 rows, 128 dimensions, 256-character payloads, 20 passes and match rates 0%, 1%, 10%, 50%, 100%. Three alternating rounds compare eager decode_row and LazyRowView; each paired checksum must agree.

This isolates decode/allocation work from storage checksums, reads, parser and TopK. A decoder improvement cannot be presented as equivalent end-to-end SQL improvement.

## Correct interpretation

Report write time, warm query time by filter category, file extent, and actual vector construction counts separately. If elapsed-time changes are noisy, say so even when construction counts drop deterministically. Retain the eager and one-key paths for regression comparisons. Do not calculate a competition E, rank or score from these local metrics.

## Revisions and cleanup

The v1 SQL and decoder results identify fa419bbda1bf63cc2ff00346f2bdcc8441274c55. They revealed repeated per-component vector conversion after validation: the lazy decoder was slower at high match rates. Commit 8f1a679ff0c4d59e13949e228cc5f9f198698578 copies validated float32 representations on little-endian hosts, retaining explicit conversion elsewhere. Both complete four-mode SQL and decoder experiments were rerun as v2. Preserve v1 as negative evidence; do not combine its samples with v2.

Both runners acquire an owned Docker container ID with create, attach inside try/finally, and remove exactly that container on success, failure or timeout. A real one-second timeout probe verified that the container and temporary data directory are removed. This is harness hardening outside the SQL timing interval. No broad Docker cleanup occurs.

The zero/zero mode is the eager/single-write reference path in the same current binary, not an untouched historical executable. Both feature switches default to enabled but remain independently configurable in the C++ API. Defaults are based on correctness, repeatable file-growth reduction and the final local experiments, not a claimed competition score.
