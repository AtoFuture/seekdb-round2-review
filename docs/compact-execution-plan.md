# Compact storage and exact scan execution plan

Goal: reduce real SQL data-page footprint and repeated page checks while preserving transactions, failures and restart behavior; then use measurements to decide exact pruning and separately test durable log commits.
Architecture: keep immutable COW roots and two-sync commits for production format03. New slotted packed pages coexist with overflow records. Format02 remains readable/writable without implicit migration. Independent recovery/log prototype never replaces production until a separate correctness gate.
Tech stack: C++20 standard library/POSIX, Alpine MiniVectorDB WSL and Docker, all artifacts D:/Codex/MiniVectorDB.
Baseline: 5ba33d2ed7d36dde6ec2e257bcb42dbff4d6b020; isolated 2026-competition-round2 checkout, no push/formal evaluation/global settings/initial branches.
Agentic workers: use subagent-driven-development, independent spec then quality reviews; parent alone owns CMake, runner, integration files and commits.

- [x] Read existing codec/store/transaction code and profiling evidence; verify clean isolated checkout and D-backed WSL.
- [x] Independent feasibility check before production implementation.
- [x] Production compact storage implementation and red/green targeted tests.
- [x] SQL integration with explicit opt-in; full regression and independent reviews.
- [x] Same-binary factorial and scale/distribution experiments, bounded scan cache and unchanged truth/reopen verification.
- [x] Choose defaults only after evidence; rebuild and run final container/runtime tests.
- [x] Evaluated conditional distance/pruning stage: distance is about4% after optimization; defer early-abandon. Page skipping conflicts with full-row validation, so keep it isolated until the integrity contract is reconciled.
- [x] Independent one-sync append-log experiment with truncation/failure/restart tests, not production WAL.
- [x] Archive raw positive/negative results, sources, Dockerfile and authentic learning material in one local tree.

## Feasibility contract
Packing changes disk addresses, so use explicit format03 magic; do not repurpose format02 bits. Constructor Store(path, index_cache_bytes=0, packed_values=false, packed_scan_cache_bytes=262144); SQL Options receives trailing fields packed_values=false and packed_scan_cache_bytes=262144 for initial experiments.
New files select format03 only when packing requested. Existing02 always stays02 and uses overflow writes; existing03 reads both packed/overflow regardless option, with option deciding future packing. Independently valid root slots (recognized magic, CRC, and root/extent bounds) with different versions reject. An invalid candidate is ignored even if its CRC is valid. No automatic migration or destructive rewrite.
Each packed page is immutable, CRC checked in full, with type3, canonical 8-byte offset/length slot directory and checked nonoverlapping bounds. Slots max508. Leaf packed reference uses high-bit marker plus (page_id << 9) plus slot. Only03 depth15 pointers may carry this marker; page_id must meet file bounds and child-before-parent ordering. Internal/overflow links remain ordinary IDs; reject unsupported tags, zero/out-of-range IDs and slots.
Small values from one bounded mutation batch share pages; oversized records use unchanged overflow chain. Flush all packed/overflow values before building index parents; publish working root only after all appends succeed. A failed partial page is never admitted to read cache.
Per-scan cache holds only validated immutable packed pages, bounded by a caller byte budget inclusive of entry metadata. Lifetime ends at scan return/exception. It must retain key order and snapshot correctness if callbacks mutate the transaction. Zero budget allows controlled read-amplification comparison. CRC/header/slot validation precedes cache admission, with address/slot checks on every use.
Single put/catalog may remain legacy overflow inside03. Current SQL row codec and full validation semantics remain unchanged; no bypass of filtered nonfinite or malformed fields.

## Ownership and test steps
Implementer owns src/storage/store.cpp, new src/storage/packed_page.hpp if useful, include/minivectordb/store.hpp, tests/packed_storage_tests.cpp, plus a narrow additive profile.hpp metric if needed and reported. Parent owns SQL Options/executor forwarding, CMake, bench, docs.
1. First write compiling legacy-compatible reduction/correctness assertions or new API compilation red; capture failing output before implementation.
2. Packed store tests compare independent map after mixed batches, signed-extreme keys/namespaces, empty and oversized values, deletion, savepoints and restore, scan callback mutations, rollback/commit/reopen. Assert fewer file pages for short-row batch and fewer actual pread/CRC on dense scan with bounded cache.
3. Corruption tests recalculate CRC for wrong counts, offsets/length overlap/out-of-range slots, tagged internal pointers, forward/out-of-range IDs and mixed root versions. Test exact boundary lengths and zero-length values.
4. Real RLIMIT_FSIZE failure at data/parent writes, recover old transaction root, retry, then reopen. Process kill tests include committed and uncommitted packed state.
5. Legacy02 created by preserved baseline binary must remain readable/writable with packing requested but no magic upgrade. New03 must read after opening without packing; baseline02 reader rejects03 rather than misreads.
6. SQL integration tests exercise multi-table CRUD, exact/filtered TopK, >128-row statements, failure after a successful chunk, overflow VARCHAR, savepoint, close/reopen and crash.
7. Parent registers core_packed_storage/core_packed_sql with finite timeouts; builds release, runs whole suite and read-only/network-none 2CPU512MiB runtime restart smoke.

## Experiments and acceptance
Preserve baseline executable before rebuild. Use same executable and fixed image/resources with format/packing/cache fields explicitly recorded. First 2000x128/40query factorial: legacy, packed cache0, packed 256KiB, packed1MiB, three alternating rounds. Include small variable payloads, production256-byte payload, long overflow payload, shuffled vs ordered keys, and update/delete churn. Large follow-up at10000 rows/200 or1000 queries; additional dimensions if justified by measurements, no simultaneous timed loads.
Compare exact result digests/full-row reopen, file growth, pread/checksum counts, CPU/wall query/write, peak memory and recovery. Read-cache metrics must distinguish requests/hits from disk reads. Repeated page checksum elimination alone is not physical IO reduction.
Keep production packing disabled if correctness fails or representative end-to-end performance regresses; fix within this turn. Reference mode remains available. Do not generalize prototype7rows/page to production256-byte text.

## Subsequent experiments and scope gates
Distance early-abandon uses positive squared partial sums in original dimension order; prune only on strict partial sum > current worst squared distance with full heap. Keep validation of every row, exact survivor summation/ties, and independent toggle. Enable only if representative performance improves.
Page-level VA skipping cannot silently weaken current validation of filtered rows. If no contract-preserving integration is available, document this gate and extend independent page-aware prototypes, not production silent skipping.
Append-log prototype self-contained bounded transaction records with sequence/length/footer checksum, one successful sync before acknowledgment, ignore only incomplete trailing record, reject corrupt committed prefix; replay exact state. Compare same payload and transaction boundaries against a two-sync reference. Add failed write/sync, truncation and restart tests. No unsafe deletion of production sync, no concurrency/group-commit benefit assumed, no production migration/checkpoint claim.

Final default decision: SQL new files use format03 and1MiB packed scan cache. Small-query256KiB wins locally, but1MiB is substantially faster at10000 rows; all exact results pass. Preserve768-dimensional ~2% negative result. Raw Store defaults remain unchanged. Final container21/21 passed96.76s and runtime PASS; raw evidence and learning record archived in the final local tree.
