# Text validation and admission evidence implementation plan

Goal: diagnose the user-confirmed failed commit 0e954 separately from current1349, then reduce repeated text validation cost without weakening validation, and make authentic learning evidence self-contained.

Authorization: the user already authorized autonomous optimization and self-review; all work stays in the existing isolated D-drive second-round checkout. No push or formal submission this turn.

Architecture: validate_text checks complete 8-byte ASCII blocks using memcpy and a high-bit mask. A successful block advances exactly eight Unicode scalar values after checking remaining character budget. Any high bit falls back to the existing scalar parser, without advancing first. Endian-independent mask, no alignment assumptions and no load beyond string bounds. A private thread-local scalar reference switch enables same-binary comparisons. No cache-validation skips, disk changes, transaction changes, new dependencies or additional memory buffers.

Alternatives: trusted-distance entry could remove repeated finite checks but changes trust boundaries; vector decoding might already compile to native loads. Both deferred until profiling justifies them. ASCII validation has a narrow shared function and can be compared against an independent scalar oracle.

- [x] Confirm failedSHA and official upstream. Rerun old public suite and old/new actual Docker numeric counterexamples. Preserve aggregate platform cause as unconfirmed.
- [x] Test first: independent scalar oracle on alignment/length/byte patterns, invalid UTF8, Unicode boundaries, limits, NUL and guard-page end. Existing selective corruption tests must still reject unprojected/filtered invalid fields.
- [x] Implement bounded ASCII fast path and scalar reference switch in private src/types header; no changes to public API.
- [x] Extend paired core harness for scalar switch and Unicode text; run three alternating rounds on dense, Unicode, wide and overflow workloads. Same data/results/file sizes/reopens; preserve negative measurements.
- [x] Review implementation, run final Docker internal/public/model/runtime checks and numeric counterexamples. Compare actual runtime with published1349 baseline; no claim of formal E/score.
- [x] Archive evidence; update mirrored learning JSON with real user/version context and exact internal JSON pointers. No invented student explanations, no automatic scoring and no credential strings.

Self-review: count never exceeds limit before subtracting; block mask valid on both endian orders; memcpy only after remaining>=8; no unchecked UTF8 bypass. Performance claims need measured controls; existing known failures on0e954 are candidates, not proof of hidden judge inputs. Learning rule scores student contributions, so agent work is labeled separately and external links are not the only evidence.
