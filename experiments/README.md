# Packed pages and exact VA-file research prototype

This is an isolated native-endian experimental file format, **not a production storage implementation**. It has no SQL, transactions, recovery, crash durability, concurrent updates, or migration support. It does not replace MiniVectorDB's format 02. No ANN, PQ, or probabilistic pruning is used.

## Contract

- 128 float32 dimensions, uint64 ID, uint32 group, 12-byte payload: 536 bytes per row.
- Three modes use the same generated rows, queries, ranking, and current `src/storage/page_checksum.hpp` CRC routine. Every requested 4096-byte page is read with `pread` and its CRC verified.
- `one_row`: one row per page. `packed`: seven rows per page. `packed_va`: the same packed data plus sequentially scanned 72-byte summaries (4-bit bin per dimension, ordinal, group).
- A 32-byte page header records magic, type, count, CRC, and page ordinal. Each file has a metadata page. Quantizer boundaries occupy five CRC-protected pages. The summary index occupies 56 rows per page.
- A query retains at most two 4096-byte page buffers, a fixed 17,408-byte boundary table, a decoded row, and O(k) results. Only the current packed page is reused; there is no full summary or database cache. Summary reads and metadata reads are included in counters. The operating system may cache files.
- Offline generation/build and independent full-sort ground truth may hold the dataset in memory; those rows are released before timing queries. The driver retains generated queries and O(queries*k) truth results. This is an algorithm memory contract, not an RSS cap or production out-of-core build claim.
- Results are ordered by binary64 squared Euclidean distance then uint64 ID. Equality checks every returned ID and the unrounded binary64 distance against the separately sorted reference, outside formatted output. Fixed dimension is a compile-time contract; variable dimensions are unsupported.

## Conservative exact pruning

Bin membership is checked during build against the exact persisted binary64 boundary values. Cells are closed intervals; their endpoints are expanded outward by one binary64 step during evaluation. Query vectors and stored rows must contain only finite float32 values. Differences, squares, and 128-term sums of these values fit binary64, including opposite float32 extrema.

Every positive subtraction, multiplication, and addition in the lower bound is rounded one representable binary64 step toward zero. Under IEEE binary64 round-to-nearest, without fast-math or fused contraction, that produces a value no larger than the correctly rounded operation on an actual contained coordinate's absolute difference. Squaring is monotone for nonnegative values; accumulation preserves this inequality by induction, so the final bound cannot exceed the exact scan's binary64 sum. Prune only when the top-k list is full and `bound > worst_distance`; equality always remains a candidate, preserving ID tie-breaking. Boundary generation rounding is harmless because containment is explicitly checked against the persisted boundaries. The quantizer is global per dimension, so uniform high-dimensional data can yield weak pruning.

Integrity is checked for pages actually requested. Skipped data pages are not read or validated; this is not a whole-file corruption scrub. CRC is accidental-corruption detection, not authentication; maliciously forged CRC-consistent summary/data pairs are outside this prototype's contract.

## Build and run

Inside the dedicated MiniVectorDB Linux environment, using only D-drive paths:

```sh
TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp g++ -std=c++20 -O2 -ffp-contract=off -Wall -Wextra -Werror -I/mnt/d/Codex/MiniVectorDB/repo/include /mnt/d/Codex/MiniVectorDB/repo/experiments/packed_va.cpp -o /mnt/d/Codex/MiniVectorDB/build/packed_va
/mnt/d/Codex/MiniVectorDB/build/packed_va --self-test --dir /mnt/d/Codex/MiniVectorDB/data/packed-va-selftest
/mnt/d/Codex/MiniVectorDB/build/packed_va --dir /mnt/d/Codex/MiniVectorDB/data/packed-va-run --rows 2000 --queries 40 --rounds 3 --seed 20261109
```

Use a dedicated output directory: the driver replaces its own `one.pages`, `packed.pages`, and `summary.pages` files. It never recursively deletes directories. `--rows 10000` supports a larger follow-up. Both uniform and eight-cluster distributions are always tested; cluster labels and group labels coincide, and filtered queries target their own cluster (a favorable correlation that is not a general workload guarantee); rows cycle among clusters, avoiding artificial cluster-sorted data locality. Queries alternate between unfiltered and group-filtered, k=10. Modes rotate order across rounds. JSONL reports logical page reads, checksum bytes, exact distance count, pruned rows, total query time, file size, and combined build time for all formats. Build time includes generation-independent encoding and write/close costs, but no fsync; it is not transactional durable-build throughput. Query timing includes file opening, reading/validation, search, result equality, and closing.

Cache scope is explicitly **warm/uncontrolled OS cache**: data has just been built and no global page-cache eviction is performed. Neither a cold physical-disk claim nor a production SQL speedup follows from these results. Data under `/mnt/d` and the dedicated distribution's D-backed Linux filesystem have different performance; record the actual path. One-row mode is a synthetic layout control, not a measurement of the production tree/row codec. Its 12-byte payload is intentionally synthetic and is not the production SQL benchmark dataset. Parent-scheduled measured results are in ../docs/portfolio-results.md and ../bench/results/packed-va-*.jsonl.

Self-tests cover duplicate vectors, reversed ID ties, empty datasets/filter results, k=0/1/10/larger-than-data, float32 extrema/subnormals/signed zero, boundary points and their adjacent floats, nonfinite rejection, CRC flips, and partial-page truncation. They also test each candidate lower bound against its independently calculated full distance.
