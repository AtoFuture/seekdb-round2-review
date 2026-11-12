# Adversarial questions, demonstrated defect and repair

This work starts from the verified row CPU optimization on local base cc0ae3f, not an older implementation. The pre-fix runtime is `sha256:6201d1c09043e83ff37a77a58b19f9615a5addfd4f6dd170a71e185fe54e77cd`. All work remains local on D:; no push or formal submission. Production changes are confined to `src/protocol/jsonl.hpp`; baseline hashes verify the prior SQL, storage, vector and row CPU implementations are preserved.

## Questions and results

| Difficult question | Result |
| --- | --- |
| Can a 65,510-byte ASCII request ID cause a write to be reported as rejected after commit? | **Yes in the baseline; repaired.** |
| Can the same response failure misreport BEGIN, COMMIT or shutdown state? | **Same demonstrated root cause; repaired.** |
| After a valid prefix and duplicate/invalid later row, can a failed INSERT contaminate an explicit transaction? | Independent SQL model passed. |
| Do heap/full/spilled sorts agree across LIMIT 64/65, ties, multiple distances, signed-key extremes and Unicode? | Independent SQL model passed with a 4 KiB sort budget. |
| Can repeated transactional table/index creation and rollback reuse stale snapshots? | 56 transaction/catalog cycles and cross-table indexed checks passed. |
| Can malformed JSON/UTF8, duplicate keys, nested unknown values or quoted delimiters corrupt the next request? | Protocol matrix and permanent regression tests passed, including exact ID lexemes. |
| Do partial writes, EINTR, zero writes, EIO and failed commit syncs leave mixed or silently unmodified state? | Baseline 472 fault cases passed; expanded final test passed 475 cases, including assertions on successful retry contents. |

No new SQL/storage defect was demonstrated in these probes. Unsupported DROP or primary-key updates were not misclassified as implementation defects. These are local tests built around declared requirements, not the hidden formal cases.

## Minimal failure and root cause

Initialize an existing database with `CREATE TABLE t(id BIGINT PRIMARY KEY)`, close it, then reopen the test process with `RLIMIT_NOFILE=4`. Its standard streams and database consume all available descriptors. Send an execute request whose ID is a string of 65,510 `x` characters and whose SQL is `INSERT INTO t VALUES(65510)`.

The baseline returns `ok:false`, category `EXECUTION`, message `cannot create JSON response spool`. A subsequent small-ID `SELECT id FROM t` returns `[[65510]]`: the insert was committed. IDs of nearby lengths demonstrate the boundary; some fail before mutation while 65,510/65,515 fail afterward. Separate probes confirm false BEGIN/COMMIT acknowledgements and failed shutdown acknowledgement after rollback.

The implementation appended the ID to its 64 KiB response buffer before executing SQL. The prefix could fit, but appending affected_rows after commit crossed the threshold and tried to create a temporary file. Its failure was then incorrectly presented as a normal rejected SQL request. This is **one root cause**, not seven separate bugs. Five pre-fix regression methods produced seven failing assertions.

The repair keeps the already validated raw ID outside the response spool. After the response payload has been fully prepared, emission writes the ID prefix followed by that payload. A normal mutation/transaction acknowledgement remains small regardless of ID length. Large SELECT payloads still spool; spool construction/write/finalization errors still return one error before output starts. Output/transport failures after emission starts remain fatal and never produce a second response. No persistence barrier, arithmetic, cache budget or stored format changed.

Resource limits and syscall wrappers apply only to isolated test processes and owned temporary databases. This is not evidence that long IDs or descriptor exhaustion caused the user's earlier formal failure; that hidden failure category remains unknown.

## Verification and review

- Pre-fix protocol matrix: 180 recorded checks, two failed state-consistency expectations; the base malformed-input/ID/Unicode matrix accounted for 160 passing assertions. State-transition evidence is retained separately.
- Pre-fix targeted regression: five methods, seven failing assertions. Initial repaired targeted run: six CTest targets passed.
- Final full Docker suite: **36/36 in 450.33 seconds**, including the added protocol, SQL model and storage fault suites.
- Review strengthened the test oracle: assert complete SQL row width, check actual data after successful interrupted-write/ordinary retries, and clean up child processes after startup failure. The three affected suites were rebuilt and rerun: **3/3 passed**. The final protocol suite has six methods; SQL model verifies 1,344 requests over 584 initial rows and 56 transaction/catalog cycles; storage verifies 475 failure/retry cases.
- Official public cases: **20/20 (95 SQL assertions)**. Additional independent model: **273 checks, 150 mutations, two kill/restarts**. Default JSONL/read-only-root/persistence/shutdown/EOF rollback smoke checks pass.
- Real-runtime comparison: **12,000 timed queries, 120 warmups, 48 extra ANN checks and 12 restarts** pass.

Runtime image: `sha256:e1cdb1e7239b915403b846f0c78ab19a4d032e58ffc77c6aba6d05552a38bc07`. Full-suite test image: `sha256:df321fb1d31642c8f0dfd567bf81613c871158c82e86370727913273170819e6`. Strengthened-test image: `sha256:82ad1cc207ad63fe02cf4e1b1ed25f8fb3b522d9e60e81061b985255899e6128`. The review changed tests only; the tested production source and runtime remain the same.

## Performance guard

Three counterbalanced rounds compare the preceding row CPU runtime against the repaired runtime: 10,000 rows, 128 dimensions, 1,000 TopK queries per sample, half filtered, two-CPU quota and 2 GiB limit. Query times include JSON serialization, pipe transport, server work and parsing. The benchmark's historical `uploaded` key denotes the supplied reference image here; that row CPU snapshot was local and not uploaded.

| Scenario | Pre-fix ms | Repaired ms | Change |
| --- | ---: | ---: | ---: |
| indexed | 3150.268 | 3110.907 | -1.25% |
| unindexed | 4751.245 | 4698.321 | -1.11% |

These approximately one-percent timing changes are treated as no observed material regression, not a new speedup claim. File sizes are unchanged. This is a warm-cache local comparison; WSL cannot enforce no-swap limits and no official score is inferred. Correctness/smoke containers use 512 MiB; timing containers use 2 GiB.

## Reproduce and inspect

The permanent tests are `tests/protocol_adversarial_tests.py`, `tests/adversarial_sql_tests.py`, and `tests/adversarial_storage_tests.cpp`, registered in CMake. The storage binary alone uses linker wrappers for pwrite/fsync; they are not linked into the production runtime. `bench/run_jsonl_versions.py` now accepts explicit reference image/labels for fair comparisons against local snapshots.

Raw old failures, passing logs, reviewed-test XML, image IDs, independent probes, runtime samples and hashes are in `bench/results/adversarial-20261110/manifest.json`. Learning records retain the actual user request, measured failure and review corrections separately from agent interpretation. Local test success does not establish hidden formal admission.
