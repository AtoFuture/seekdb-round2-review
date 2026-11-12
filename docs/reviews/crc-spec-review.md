# CRC32 specification review

Verdict: PASS for implementation commit 6f1824f48673fea71da59d9a20719bd25e4fe12c against 4ad8b99326cbd24523912143bff58b2e286a3c78 and docs/checksum-plan.md. No blocking findings.

Reviewed the complete four-file commit diff and the current uncommitted combined-only runner change. The constexpr 256-entry table uses reflected IEEE polynomial 0xedb88320, initial 0xffffffff and final complement. Each of all 4096 page bytes is processed, with exactly four bytes zeroed at the caller-provided checksum offset. All four store call sites retain root offset 32 and data offset 24. The change leaves format identifiers, layout, validation order, read/write behavior and fsync logic unchanged. It uses portable unsigned arithmetic and no CPU-specific instructions or external dependency.

The independent bit-at-a-time test oracle checks a known zero-page CRC, uniform patterns, incremental bytes, all 4096 individual byte positions, and 256 deterministic random pages at both offsets. Tests cover input nonmutation, ignored checksum fields, adjacent/final-byte sensitivity, on-disk page checksums and committed data reopening.

Independent focused validation: ctest --test-dir /mnt/d/Codex/MiniVectorDB/build/core -R '^core_checksum$' --output-on-failure: 1/1 PASS, 0.55 seconds (0.56 total). No benchmarks or broad regression reruns performed. An initial attempt used the nonexistent repo/build/core directory and executed no test; the corrected project build/core path passed.

Benchmark readiness: --combined-only retains only decode1-batch1 from each existing schedule; SQL implementation/source and runtime command are unchanged. The runner accepts one binary, so old/new alternating order must be orchestrated outside it, for example three one-round pairs A/B, B/A, A/B. Use distinct output paths, the same optimization.cpp source hash, identical rows/queries/image/resources, preserve binary hashes, and compare result digests across both versions. Actual timing fairness/results remain pending execution, not claimed by this review.

Read D:/Codex/MiniVectorDB/AGENTS.md. No nested repo AGENTS.md was found. No source changes, commits or pushes. All test temporary files were directed to D:/Codex/MiniVectorDB/tmp.
