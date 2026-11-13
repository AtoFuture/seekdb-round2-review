# Independent SQL milestone code-quality review

Date: 2026-11-09
Verdict: READY = YES for this local development SQL milestone. No blocking correctness or quality findings.

## Reviewed snapshot and scope

Repository: D:/Codex/MiniVectorDB/repo, second-round workspace only.
Baseline: 78a15be0789f88dd201b7670a282380e26473f7a.
Library HEAD: 7c140e88e167aaef6255f0e509ed980040f414f7.
Included the pending CLI src/main.cpp, CLI/runtime tests, Dockerfile, CMakeLists.txt, scripts/dev.ps1, README and protocol/research/plan documentation, and both learning JSON files. Final validation documentation was being finalized by the parent; container results reported by the parent were not substituted for my own checks.

Read the actual SQL lexer, parser, binder, executor, sorter, catalog, binary codec, shared-root store and relevant vector/value code. This is a code-quality review of the agreed subset, not a claim of official protocol compatibility, power-failure certification or performance qualification.

## Findings

No actionable blocking findings were identified. The earlier VARCHAR comparison-literal width correction is present and separates stored-value validation from predicate validation while still validating UTF-8.

Points specifically examined:

- Store scans retain immutable page references while UPDATE/DELETE replace the working root. Namespaces share one durable root; cross-table and catalog changes therefore use the same savepoint/publication path.
- executor.cpp:193-224 restores the statement savepoint on mutation failure; automatic transactions then roll back, while explicit transactions retain earlier successful statements. Commit errors use the separate poisoned-state path at lines 225-235 instead of restoring potentially published data.
- Bind-time validation covers projection/order expressions, constant assignments, duplicate insert keys, all supplied columns and unsupported vector predicates before mutation or LIMIT 0 short-circuiting.
- Distance caches hold pointers only to stable bound expressions during the current call; heap winners retain keys and distances rather than callback-owned row views. Heap ordering and external sorting use squared distances and primary-key ascending ties consistently.
- sorter.cpp uses two merge inputs and one output, indexed run filenames instead of an in-memory run list, checked binary lengths, and query-owned unique temporary directories. Cleanup on normal completion and exceptions is tested. Soft budget/current record/stream overhead and process-kill temporary-directory leftovers remain documented limitations.
- CLI framing handles comments, doubled quote/backtick escapes and incomplete EOF frames; transport framing and typed output are explicitly local dev-tsv. Successful mutation responses are emitted after execute/commit returns. Runtime stage contains the executable and libstdc++, with Python confined to builder/test tooling.
- Learning JSON parses and both copies matched at review time. Source labels distinguish user statements, tool summaries and agent observations; no new evidence was fabricated by this review.

A transient whitespace-only issue in docs/environment.md was sent to the parent and fixed during final documentation normalization; it is not an outstanding review finding.

## Independently executed verification

1. Existing D-drive Linux build, dedicated test root /mnt/d/Codex/MiniVectorDB/data/quality-tests, TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp:

   ctest --test-dir /mnt/d/Codex/MiniVectorDB/build/core --output-on-failure -R 'core_(codec|sql|sort|namespaces)|cli'

   Result: 8/8 passed in 7.98 seconds. Included namespaces, codec, SQL, external sorting, SQL crash, edge cases, actual write failure and CLI. CLI includes its seven black-box cases, 90-step model and acknowledged-commit SIGKILL case.

2. Independent failure probe, kept outside the repository:

   D:/Codex/MiniVectorDB/work/quality-atomic.cpp
   D:/Codex/MiniVectorDB/work/quality-atomic

   Compiled against the current static library and executed successfully. RLIMIT_FSIZE forced real write failure after enough pages for an earlier row/catalog write:

   - Autocommit UPDATE: permitted 18 additional pages, failed later in the statement, retained all original values, and left no active transaction.
   - Autocommit DELETE: permitted 17 additional pages, failed after earlier deletion work, retained all three original rows, and left no active transaction.
   - Explicit CREATE TABLE: permitted 18 additional pages so table metadata could be written before the catalog counter failed; the new table disappeared, an earlier transaction INSERT remained visible, and the transaction stayed active.
   - After restoring the resource limit, another table could be created and populated, COMMIT succeeded, reopen preserved both committed tables and data, and the formerly failed table name could be created successfully.

   Actual output: PASS independent autocommit UPDATE/DELETE and explicit CREATE I/O failure rollback, continuation, commit and reopen

The independent probe removed only its own mkdtemp-created test-data directory after passing. No repository source edits, commit, push, formal evaluation, initial-round changes, C-drive project writes or global WSL changes were performed by this reviewer.

## Limits of this approval

Approval covers the stated development milestone and inspected snapshot. It does not remove the documented radix write amplification/no reclamation, full-row decode, soft sort budget, orphan sort directory or missing official-protocol/ANN limitations. No exhaustive fuzzing, power-cut simulation or benchmark scoring was performed. Parent remains responsible for recording final full-container/runtime evidence and committing the reviewed pending integration files.
