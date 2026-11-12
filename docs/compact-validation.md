# Compact production validation — 2026-11-10

Final production tree is measured core b7eb52b4bb8fbc25e3209d0933182e57e41afdd8 plus SQL defaults packed_values=true and1MiB packed scan cache. Benchmark arms explicitly override both fields. No other production changes after measurement.

Fresh Docker build and complete suite: **21/21 passed,96.76 seconds**. Runtime smoke: **PASS**, read-only root, network disabled,2CPU/512MiB,/data persistence and EOF rollback across restart. Test data uses D-drive bind paths; timed data uses Linux filesystem inside the D-backed MiniVectorDB distro disk. No global WSL settings changed. These are local constraints, not official resource admission.

Test image: sha256:57e770894ff1dc2f373315aa3073c03a1e642e09ecfbf2ee832b00252c074ec6.
Runtime image: sha256:f13894c3a9c42d574a5c31b2ab61e95dc4a0c383f95572cf9d2b79238c0da620.
Docker did not embed Git metadata because its build host lacks Git; image identifiers were taken from successful build logs.

## Evidence and coverage

- Initial production integration:21/21 tests passed107.68s; singleton/cache-copy refinement affected suites4/4 passed39.75s; final defaults then verified by the fresh complete Docker suite above.
- Storage tests: independent map, empty/exact boundary/overflow lengths, signed keys and namespaces, nested/mutating scan callbacks, savepoints, rollback, eviction, tagged-address and CRC-valid malformed pages, invalid root fallback, real RLIMIT_FSIZE partial data and index writes, retry and SIGKILL recovery.
- SQL tests: independent filteredTopK answers and eight combinations of decoding/batching/cache, multi-table operations, chunks exceeding128 rows, long text, malformed packed row rejection, calibrated later-chunk failure preserving prior transaction, reopen and process termination.
- Red/green refinement: singleton-packing assertion failed on the prior implementation and passes after fallback. Historical format03 single-slot fixture remains readable with packing disabled, followed by new commit and another reopen.
- Preserved baseline binary SHA256:533b2313d4d1b776a61324f2d9a7f72517fd3040f161f5d24f06b91da644c91b. It created02, current code appended with packing requested, baseline read all keys; format stayed02. Current03 was rejected by baseline startup with its generic corrupt-file error. Probe ran before refinement; historical03 compatibility and final regressions cover the refinement.
-156 compact benchmark samples,8,016 validated queries and156 complete reopen checks, including smoke, diagnostic, v1 and v2. These are local repetitions, not distinct official test cases. No output discrepancy; unfavorable timings are retained.
- Append-log selftest passed both modes: every-byte truncation/corruption, structurally invalid CRC-correct frames, payload bounds, locks, injected write/sync failures and SIGKILL. Six controlled benchmark records pass full live/replay map equality. Source unchanged across measurement verified by SHA256; runner now checks before/after hashes.
- Independent SPEC and QUALITY reviews pass, including final default tradeoff. Reviewers inspect source; execution evidence comes from parent-run tests. Reports in docs/reviews/compact-*.md and append-log-spec-review.md.

Formal MySQL/competition driver is still unavailable in the current workspace; this turn did not recheck remote publication. No official admission, score, power-loss certification, ANN, migration or space reclamation claim. Production retains its two-sync commit protocol; append-log experiment does not replace it. Full row validation remains unchanged, so page-skipping VA remains isolated.

Raw logs reside under D:/Codex/MiniVectorDB/work. Artifact hashes follow.
- compact-final-image.log: 4e81cc38e4b9fa9db414d5c02e5183bd9a146b4220e17e99226b294e9951a5ba
- compact-final-container.log: 0712937143f04a935a4ade0f602237d2b73d6663154b683c618d918566a0d31d
- compact-final-runtime-build.log: 518bc3802576a19cc22b0ca83456a83684cfada448db0cddee3269a393bbf18d
- compact-final-runtime.log: 8dba047475731f9a65f884e04d1ba719005d58b52171d9ce23655045913f4232
- compact-v2-targeted.log: 1f7ff4861fdad66e74413313d776a269f4e0facded65c2a57e5c3674c37d6e1f
- compact-compat.log: d7e6afaf467d7c66ece6ddfbbad2c25cf70e24c56796a0884c886634dd0f177a
- append-log-final-selftest.log: 5222c154632b09899fd5cf3bf1dd25d059928f3571ed460a384eba6d68e53eac
