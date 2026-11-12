# Page checksum optimization plan

User authorized continued local optimization. D drive only; no push/formal evaluation/global WSL or initial branch changes. Baseline 4ad8b99326cbd24523912143bff58b2e286a3c78.

Hypothesis: bitwise IEEE CRC32 over every page costs significant CPU. Replace eight bit iterations per byte with a fixed constexpr 256-entry lookup table, preserving polynomial, initial/final XOR and zeroed four checksum bytes. No checksum bypass, caching, format, durability or query changes.

1. Independent bitwise oracle over page patterns/random bytes and root/data offsets. Preserve corruption and reopen behavior. Run regression tests.
2. Specification then quality review.
3. Preserve old SQL binary before rebuilding. Use identical benchmark source/data, selective decode and batch writes enabled. Compare old/new binaries in alternating rounds, three samples each, 2000 rows/40 queries, same runtime image and resources. Record exact results, reopen checks, hashes and source versions. No competing project test/build load during timing.
4. Report medians/ranges and limitations, retain all raw measurements, append authentic Agent learning evidence, local commit/checkpoint only.

The added --combined-only runner flag selects mode11 from each established schedule; it does not alter timed SQL API behavior. This is a two-version comparison, distinct from prior four-mode factorial measurements.