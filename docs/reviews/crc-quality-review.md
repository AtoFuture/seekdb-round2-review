# CRC32 quality review

Verdict: PASS. No blocking implementation or runner findings.

Scope: read-only review of baseline 4ad8b99326cbd24523912143bff58b2e286a3c78 through implementation 6f1824f48673fea71da59d9a20719bd25e4fe12c; complete four-file diff, current combined-only runner diff and runner context, work/run-checksum-comparison.py, and independent work/crc-spec-review.md. Project AGENTS.md was read. No tests, builds, benchmarks, source edits, commits or pushes were performed by this reviewer.

The constexpr 256-entry table and byte recurrence implement the same reflected IEEE CRC-32 polynomial 0xedb88320, initial 0xffffffff, and final complement as the prior bit loop. Unsigned shifts and indices are bounded. Every one of the 4096 bytes contributes; exactly four checksum-field bytes are treated as zero, without modifying input. Existing root calls retain offset 32 and data calls retain offset 24. The private helper only receives these valid fixed offsets. Format 02 layout, validation order, write publication and fsync/durability behavior are unchanged.

Tests use an independent branch-based bitwise reference, a fixed zero-page checksum, uniform and incremental patterns, all byte positions, deterministic random pages, both offsets, nonmutation, checksum-field exclusion and adjacent/final-byte sensitivity. On-disk page checksums and reopening committed values are covered. Existing focused and full-suite passing evidence was supplied by the parent/spec report; it was not rerun here.

The combined-only filter retains precisely decode1-batch1 while leaving existing benchmark commands and constraints intact. The external comparison script executes six sequential runs in A/B, B/A, A/B pairs with three samples per implementation, the same pinned image, rows, queries, and runner. Outputs are distinct; the runner rejects overwrites and records executable and benchmark-source hashes. No concurrent benchmark workload is introduced.

Nonblocking validation caveat: the comparison wrapper does not itself assert equal result_digest values or benchmark-source hashes across its six files. The child runner's digest assertion covers only its own records (one per invocation here). Cross-file equality must be checked before interpreting timings. The parent reports that six outputs have matching result digests, benchmark source hash, image, database size and work counts, satisfying that requirement for this experiment. Performance measurements and that cross-file check were not independently rerun in this review.
