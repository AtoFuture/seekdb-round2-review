# Independent portfolio specification review

Baseline: 65423b15609b858e20ebd6123c9158ab4db9cd17. Reviewed the current uncommitted production diff and new tests, bench/portfolio.cpp, bench/run_portfolio.py, experiments/packed_va.cpp and experiments/README.md against AGENTS.md and docs/portfolio-plan.md.

## Verdict

Production specification: PASS. Prototype specification: PASS within its explicitly documented experimental scope. No blocking correctness or specification defect found by source inspection. This is not a performance conclusion or a claim of independently executed tests. The parent reported 19/19 tests passing in 69.48 seconds; this reviewer ran no builds, tests or benchmarks, to avoid interfering with measurements.

## Production evidence

- Store owns its fixed CacheEntry array. The requested byte budget bounds the allocated entry array including page data, ID/depth metadata and padding. Fixed Store bookkeeping and allocator overhead are explicitly excluded by the API contract. Budget zero disables the cache, and Database Options forwards the budget correctly.
- Only INDEX/branch reads enter the cache. Root slots use raw reads; overflow pages remain uncached. Cache admission follows magic, checksum, type, expected-depth and all child-pointer validation. Hits check address range, matching ID, expected depth and type. The returned Page is a copy, preserving scan snapshots while callbacks mutate the store.
- No append-time cache admission occurs. next_page advances only after a complete page write; a failed write cannot leave a cached entry at the retried address. Rollback/savepoint retain immutable old addresses. Reopen allocates the cache only after durable-root validation and begins empty.
- CRC slicing tables retain the reflected IEEE polynomial, initial/final XOR and four zeroed checksum bytes. Explicit byte assembly avoids host endian/alignment requirements. Bytewise remains selectable per thread in the same binary; the wrapper profiles exactly once. Page offsets 24/32 remain unchanged.
- The new cache tests cover disabled/subentry budgets, bounded residency/eviction, overflow reads, savepoint/rollback, mutation during scan, reopen, partial-write retry and malformed-page rejection. CRC tests use a separate bit-at-a-time oracle, every valid field offset, patterns/random pages/single-bit positions, ignored checksum field changes and profiling/thread-selector behavior.

## Production benchmark fairness

The runner uses the same binary and pinned image, fixed CPU/memory limits, identical seeded data and queries, six bytewise/slicing-by-8 x 0/256KiB/1MiB modes, alternating/reordered rounds, and separate diagnostic invocation. Exact row/order/distance truth and full reopened-row comparison are performed outside recorded query/write times. The runner verifies result digest, file-size/work invariants and binary stability. Cache counters are intentionally active in both timing and diagnostic runs; detailed phase profiling is disabled for timing runs.

The runner records benchmark-source and executable hashes; the parent still needs the planned complete source manifest, measurement evidence, larger comparison and Windows-path regression before selecting defaults or making final performance claims. These are execution deliverables, not defects in the inspected harness.

## Prototype evidence and limits

Three standalone disk layouts preserve identical 536-byte records and exact binary64 squared-distance/ID ranking. Page metadata, summary/boundary reads, requested-page checksum bytes and data reads contribute to counters. Query storage is bounded by two page buffers, fixed boundaries, one decoded row and O(k) results. Offline dataset/truth materialization and query/truth retention are clearly distinguished from query algorithm memory.

Lower-bound endpoints expand outward. Positive subtraction, squaring and accumulation move one representable binary64 step toward zero; with finite float32 coordinates, round-to-nearest and contraction disabled this preserves conservativeness against the scan's accumulation. Only bound strictly greater than the worst distance prunes, preserving ID ties. Boundary containment is checked against persisted boundaries during construction.

Self-tests cover exact full-sort comparison, tied/reversed IDs, filters, k edges, empty data, extrema/subnormals/signed zero, boundary-adjacent values, invalid nonfinite values, CRC and truncation. Persisted row roundtrips include payload and float bits. Native endian, no crash/recovery/SQL integration, warm uncontrolled OS caching, synthetic one-row control and corruption validation only for requested pages are explicitly disclosed. No production SQL speedup follows from this prototype.

## Additional tests worth adding (nonblocking)

1. Construct a CRC-consistent malformed tree where an INDEX page is first warmed at its valid depth and then reached at a different expected depth through another reference. Existing malformed-depth tests exercise admission rejection, not the cache-hit rejection branch. This is the highest-value additional adversarial cache test.
2. Keep two Store objects on distinct files live, with identical page IDs but different values, and interleave cached reads. Source ownership is correct; a direct isolation regression would protect this requirement.
3. Exercise cache-enabled multi-row batch failure followed by statement/savepoint restore and retry. Existing cache tests test single-key write failure and existing batch tests largely use the default zero cache.
4. Add a deterministic exponent-distributed randomized lower-bound differential test (including tiny differences atop large float magnitudes) to supplement the prototype's current extrema and bin-boundary tests. No counterexample was found in source reasoning.

No source files, global settings, commits or remote branches were modified by this review.
