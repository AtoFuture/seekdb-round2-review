# Admission fixes implementation plan

Goal: repair the two reproduced admission gaps without changing durability or initial-round branches. User authorized execution of diagnosed fixes. All artifacts stay under D:/Codex/MiniVectorDB; no push or formal submission.

Evidence: upstream f4d4e26653af823a7601668802f80e374ad6643c now explicitly defines JSONL requests/responses, exact ready object, execute/search_mode and shutdown. This supersedes ambiguous older raw-SQL wording. Existing dev-tsv cannot pass. Public v4 also reproduces missing CREATE VECTOR INDEX.

Design: native C++ JSONL adapter, preserving dev-tsv only as an explicit development option. No subprocess bridge. Preserve integer IDs and BIGINT exactly, validate framing and JSON, spool large SELECT responses under data directory before emitting so execution errors produce one response without unbounded memory. Default Docker entrypoint selects official protocol. Shutdown rolls back before acknowledgment.

Index: implement a real persistent, bounded-memory flat vector index in Store namespaces, metadata and rows committed under the same root. This exact index is also a valid exhaustive search implementation for ann (recall 1); do not claim approximate pruning. Maintain inserts/updates/deletes, statement atomicity, rollback and reopen. Validate indexed column/type and duplicate names. Query actually uses index, with predicate checks and stable primary-key ties. Preserve current format02/03 compatibility.

- [x] Capture official files and inspect Python driver; reproduce old protocol failure with bounded timeout.
- [x] Add failing protocol tests; implement adapter, Docker default and runtime smoke. Preserve signed64 and Unicode, one response on errors, EOF/shutdown rollback, exact/ann explicit routing.
- [x] Add failing vector-index tests; implement catalog/parser/index maintenance and query path. Test build, edits, rollback, failed statement, restart and bounded processing.
- [x] Independently review spec compliance and implementation quality, resolve findings.
- [x] Build actual Docker runtime, run unmodified official handshake/public suite, full internal regressions and crash/rollback protocol tests. Report public-only scope honestly.
- [x] Record diagnostics, validation and real learning evidence. No push, no formal submission.

Alternatives rejected: output-only JSON cannot parse official requests; no-op index would hide a missing required subsystem; unbounded in-memory vector copy violates resource requirements. New protocol doc is authoritative; driver is useful but is reviewed because organizer explicitly says it was AI-written and not thoroughly run.
