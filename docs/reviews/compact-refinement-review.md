# Compact refinement reviews

Independent SPEC and QUALITY reviewers both returned PASS with no blockers.

Singleton flush passes the builder payload through ordinary format03 write_value before resetting the builder, preserving empty values, append-before-reference ordering, atomic working-root publication, and retry semantics. Cached page references are copied into owned strings before callbacks; physical-address and slot validation remain, so recursive scans and callback mutations cannot invalidate returned data. Tests cover singleton boundaries, individually unpackable rows, packed groups with an ordinary tail, and historical packed-singleton get/scan followed by new commit and reopen.

Reviewers performed read-only inspection, not independent execution. Parent execution: core_packed_storage, core_packed_sql, core_batches and core_sql_failure all passed (4/4, 39.75 seconds). The singleton assertion failed against the prior implementation before the change.

Append-log runner nonblocking feedback: hash source before running and verify unchanged afterward, like its binary. Parent applied this provenance improvement; existing measured source was frozen and raw records are retained unchanged.

Final default review: QUALITY PASS, subject to final tests. Reviewer verified small/large improvements52.59%/50.12%, explicit benchmark option overrides, raw Store unchanged, format02 compatibility, format03 reader requirement, cache accounting, and append-log source hashing. Reviewer requested the varied table and explicit768-dimensional315.298->321.822ms (+2.07%) with unchanged file size; parent added both. No independent execution by reviewer.
