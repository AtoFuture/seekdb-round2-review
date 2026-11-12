# Optimization portfolio execution

Baseline65423b15609b858e20ebd6123c9158ab4db9cd17. User authorized implementation and autonomous fixes, all D, no initial branches/global settings/push/formal evaluation.

Production candidates:
A. Bounded validated index-only page cache, 0/256KiB/1MiB, initial default0. Per-store ownership; full validation before admission; hit type/depth checks; no overflow/root-slot cache; bounded payload+metadata, counters. Preserve immutable snapshot/savepoint/failure semantics.
B. Portable CRC slicing-by-8, bytewise mode retained in same binary. Exact same IEEE polynomial/initial/final/checksum-zero bytes/format02 and per-page profiling. No hardware ISA requirement or CRC32C substitution.
Independent source ownership; parent coordinates CMake/builds/commits/reviews.

Research prototype:
C. Separate disk-backed one-row/page vs compact multiple-row/page vs compact+VA interval-summary exact TopK. Bounded query page buffers, independent exact truth, uniform and clustered datasets, ties/filter/float-boundary/corruption checks. Prototype explicitly excludes SQL/transactions/crash integration. Do not replace production format until evidence and recovery design justify it.

Validation:
TDD targeted tests then full container regression/runtime, independent spec then quality review.
Production native-Linux D-backed WSL benchmarks: same binary/image/data/resources; 2 CRC modes x3 cache budgets x3 alternating rounds at2000x128/40queries. Separate stats sampling from uninstrumented timings; raw data retained. Select defaults only after results; run larger workload comparison and Windows-path compatibility smoke.
Prototype benchmarks separately attributed, no combined "production speedup" claim; retain negative outcomes and fallback exact scans. Source/binary hashes, exact results, file bytes, read/checksum counts, build and query costs, memory limits.
Archive complete sources/Dockerfile/authentic Agent evidence in one local tree and checkpoint.

Longer-term path compression, base+delta architecture and ANN remain conditional follow-ons: user approved research-backed direction, but production integration requires separate design/results rather than speculative full rewrite in this phase.
