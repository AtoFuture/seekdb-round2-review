# Independent append-log specification review

Decision: PASS. No specification blockers found.

Scope: read-only review of experiments/append_log.cpp, experiments/append_log.md, and the independent append-log contract in docs/compact-execution-plan.md. No tests or benchmark loads executed by this reviewer. No production source edits, commits or pushes.

Acknowledgment and failures: append returns only after its final successful fsync and state application. Any exception after I/O begins poisons the writer; preflight validation rejection does not. Reopen may recover a complete unacknowledged frame, which is correctly documented as uncertain completion rather than rollback. File and parent-directory setup synchronization precede timed transactions.

Replay and corruption: header recognition, checksum, length/count limits and consecutive sequence checks precede frame-sized allocation. Frame/footer checksums and all operation semantics are validated before applying a transaction. Construction failure exposes no partially recovered Log instance. Only incomplete final header/frame tails are truncated and synchronized; complete invalid headers/frames fail closed. Documentation correctly acknowledges that corruption resembling an incomplete tail is inherently ambiguous and CRC is not adversarial integrity protection. Transaction-sized allocations are bounded; total recovered state intentionally grows with live contents.

Measurement: one-sync and two-sync modes use identical records, payloads and transaction boundaries. Workload and expected state are prepared before timing; mode order alternates by round. Live and reopened maps are checked exactly. Timed writes include identical framing/CRC and map application, with one-sync additionally coalescing the footer write. This is explicitly described as a combined write/sync strategy comparison. Setup and recovery synchronization are excluded from transaction counters, replay and full reopen timings are distinguished, and immediate reopen is not presented as cold-cache evidence.

Scope boundaries: isolated executable is not linked into production. Documentation makes no production WAL, checkpoint, migration, garbage collection, index-maintenance, group-commit, concurrency, SQL-throughput or power-loss-proof claim. Injected sync errors occur before the syscall; process SIGKILL and injected faults are correctly limited to control-flow/process-recovery evidence.

Validation evidence: parent reports final -O2 -Wall -Wextra -Werror compilation and --selftest passing, including truncation/corruption sweeps, write/fsync injection and SIGKILL. This review did not independently execute or verify the reported test log. Parent owns forthcoming serialized measurements and environment labels.
