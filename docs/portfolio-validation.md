# Portfolio validation — 2026-11-10

Final tree: measured core46c92cb156b5f4f9c96ff88dd2a87165761f5f6a plus SQL Options default1MiB. Benchmark sets cache explicitly, so this default change does not alter measured arms. No other production changes after measurement.

Fresh Docker test image: sha256:2bd204dd55787f9ff252b56a1cc35e086c413460ba9d4c385cc01300c2c58e1d. Runtime image: sha256:a5cfe6055ddcf5a5d88c1e89bb17c685f3efa1e46d6b1b8696982716f56b985f. IDs extracted from successful build logs; source Git metadata was not embedded by Docker because its build host lacked Git.

19/19 CTest passed,59.30 seconds, including CRC oracle and bounded cache adversarial cases. Runtime smoke PASS: read-only root, network disabled,2CPU/512MiB,/data persistence and EOF rollback across restart. Windows D-bind test data and D-backed Linux benchmark data both used. No new global settings. Swap-limit unsupported; not official resource admission.

Cache cases include rollback/savepoints, mutation during snapshot scan, partial write retry, reopen empty, malformed pages, wrong-depth cache hit without IO, and independent Stores with colliding page IDs. CRC oracle checks every valid offset, patterns/random data and profiling/TLS. Prototype self-tests cover ties/extrema/subnormal/filter/k/nonfinite/CRC/truncation and bound conservativeness. Spec and quality reviews PASS by source inspection; parent executes tests. Sanitizer attempt unavailable because dedicated distro lacks sanitizer libraries.

Formal protocol/MySQL suite still absent; no claim of formal correctness admission, power-loss guarantee or score. Prototype excludes SQL/transactions/recovery. Cache assumes cooperative exclusive ownership; external mutation of open files unsupported. Compaction/ANN remain unimplemented.

Raw parent logs in D:/Codex/MiniVectorDB/work; SHA256:

- portfolio-final-image.log: 576f3fb4207b129ca0f95181323439a81b77ce7e7f4b1d19b8ee53d261e7ca40
- portfolio-final-container.log: 697c8c3ece662f4691dfda4d20e54718f1b058478319979401651da9bfa6bac2
- portfolio-final-runtime-build.log: 5839d25317b65392ee35673a4ef65c2cb3c860f841a0cd18b2d1f841f1a3787d
- portfolio-final-runtime.log: 8dba047475731f9a65f884e04d1ba719005d58b52171d9ce23655045913f4232

Final rebuilt prototype self-test: PASS. Raw log SHA256 789b100009a6aa7a43c50db2e9d69a51abb8af15492b020fd347d2ebe1c45e43
