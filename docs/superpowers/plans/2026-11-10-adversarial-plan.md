# Adversarial correctness work on the latest optimized tree

User request: devise difficult database questions, execute them, and repair demonstrated defects. Preserve the preceding row CPU optimization. All artifacts stay on D:, with no push, formal submission, initial-round branch change, or global settings change.

## Approach

Use three complementary methods: independently modeled SQL sequences, malformed/valid protocol alternation, and targeted storage/failure injection. Random bytes alone have poor reach into semantic states, while only example-based tests miss interactions; combine deterministic boundary examples with seeded valid-state transitions. Unsupported SQL is not a defect. A candidate must have a reproducible expected/actual mismatch and a documented invariant before changing production code.

## Questions to test

1. Can invalid JSON, duplicate keys, malformed Unicode, deep ignored values or oversized input contaminate the next request or prematurely terminate the service?
2. Can a failed multi-row statement partially alter an explicit transaction, index or warmed scan snapshot? Are earlier successful changes still committable?
3. Do exact/ANN, indexed/unindexed, TopK/full sort/spilled sort agree at signed BIGINT extremes, Unicode/NUL text boundaries, signed zero, subnormals and rounded-distance ties?
4. Do index creation, update, rollback, commit and restart preserve identical row/vector sets?
5. Can response-buffer/spool failure incorrectly report a committed write as rejected? Which failures are application errors and which imply an unknown commit outcome?
6. Can crafted/torn/truncated pages, failed writes or recovery interruption make acknowledged state disappear or allow stale/cross-transaction savepoints?

## Completion gates

- [x] Record attack matrix and baseline observations, including passing cases.
- [x] Convert concrete defects into tests that fail on the pre-fix implementation.
- [x] Apply minimal root-cause fixes and rerun the exact failing tests.
- [x] Independent review of fixes and test assertions.
- [x] Full Docker internal/public/model/crash/runtime regression on the newest combined source.
- [x] Compare performance against the preceding row CPU runtime where the fixed path is relevant.
- [x] Archive evidence and authentic learning records; distinguish local coverage from unknown hidden formal inputs.

Results: [adversarial report](../../adversarial-results.md).
