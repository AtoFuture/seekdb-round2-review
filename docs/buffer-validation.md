# Data-buffer validation — 2026-11-10

Measured implementation250375570ff8b4f4db747b6ab6dc5b667163b9e1, followed only by SQL defaults16MiB data cache and scan memo0. Benchmark controls both explicitly. Raw Store defaults unchanged; no production format or commit protocol change.

API-red captured before Store and SQL implementation. Standalone storage build uses -O2 -Wall -Wextra -Wpedantic -Werror; passed. Parent Release full suite23/23 passed115.44s. After selecting defaults, fresh Docker suite **23/23 passed123.87s**; runtime smoke **PASS**, read-only root/network none/2CPU512MiB,/data persistence and EOF rollback across restart. This is local validation, not official admission or a power-loss certification.

Final test image sha256:add7d5201648fd46c72775d00d8da80e0f0304813a29875d0919f6ee892c7b8d.
Final runtime image sha256:ac93c9b166ed664c75cc1f091a94b375025d6f19f1ba42781cd40ffdf1c50a67.
Image IDs extracted from successful build logs; Docker host lacks Git so automatic Git metadata was not embedded.

New storage coverage: zero/subset/full budgets, lazy allocation, repeat get/scan without new pread/CRC, four-way recency/eviction, independent stores with matching addresses, savepoints/rollback, nested mutating callbacks, closed-file invalid CRC and CRC-correct malformed structure, wrong-type cached references in both directions, cached slot bounds, three actual RLIMIT_FSIZE partial-write offsets with retry, SIGKILL and reopen.

SQL coverage:32 option combinations with independent whole-row/filteredTopK answers; repeat-query validation counters, committed changes and rollback/reopen. Eight real partial-write statement failures verify restoration of prior transaction data, after successful chunks and an actual partial write. Nonfinite vectors and invalid UTF-8 in CRC-valid packed/ordinary/overflow pages remain rejected on a second query with confirmed data-cache hits. Duplicate-primary-key preflight is separately labeled; it is not used as evidence of savepoint restoration.

Independent feasibility, SPEC and QUALITY source reviews PASS; final default review also PASS. Reviewers did not execute tests. Parent executes all reported validation and controlled measurements.

77 controlled samples:5480 main query checks,77 first-query checks and77 full reopened maps, no discrepancy; all cache modes preserve file bytes. Three-round small/large/varied/overcapacity groups, separate diagnostic group. Baseline pre-implementation diagnostic uses preserved oldb7 executable; its explicit options match uploadedbdae baseline behavior. Five-arm smoke has20 main checks,5 first checks/5 reopens and working-tree source label, excluded from speed claims. Exact method and negative cases in buffer-method.md and buffer-results.md.

No global settings, initial branches or remote refs changed this turn. All project artifacts are D-backed. Previously uploaded remote remainsbdae610c073a215d1fe85727ef85a2a8062ea238. Formal protocol/driver still not available locally; remote package publication was not rechecked this turn. ANN, migration and space reclamation are not added.

Raw parent logs under D:/Codex/MiniVectorDB/work; SHA256:
- buffer-build.log: cc46aef621d556b922e85b71f98514f9188991c973bf4126a757a112290967be
- buffer-tests.log: 4c63e46109d8002ef2ab27d445c30441dea1cb7dd8d23092d8553eb8bebb521c
- data-cache-sql-red.log: b4cd0763629253421f1fdb22792fcd27b82ee3dbf0d6e63b9d9132005368ed7d
- buffer-final-image.log: a592bde14820558c90dbf88184d11ddfef324676f6e776b0fd3f492d5d8bf412
- buffer-final-container.log: e313c2e633b73b773894654fbc431fcd2344da26ed965edbf33304bac5502ab9
- buffer-final-runtime-build.log: 94c939f4ff434b7bc1dae0362782acb44ac0697cefa18b7b7b35a627f8e31c6a
- buffer-final-runtime.log: 8dba047475731f9a65f884e04d1ba719005d58b52171d9ce23655045913f4232
- buffer-smoke.jsonl: 531bef0080c3365dcdb9cd5d83582d74f4b1d360ed6a5526553016ca9f23e1b1
