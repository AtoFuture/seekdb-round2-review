# ASCII text validation: bounded optimization and evidence

Base: uploaded `1349fd33b1be6e225ab64601b73e390a839241ca`. This snapshot packages the verified changes for the user-authorized GitHub upload; no formal evaluation has been submitted by the agent. Admission diagnosis is separately recorded in [reported-admission-failure.md](reported-admission-failure.md): the user-confirmed failed SHA was the older 0e954, not this base.

## Design

`validate_text` still validates every character and enforces Unicode scalar length. On the optimized path it loads eight bytes with `memcpy` only when eight bytes remain. If no byte has its high bit set, all eight are ASCII scalars; the code checks `limit-count >= 8` before advancing. A mixed block advances nothing and falls through to the unchanged scalar Unicode validator. The repeated-byte bitmask is endian-independent. There is no extra heap buffer, validation cache, disk-format change, approximate result or persistence change.

A private thread-local `detail::text_validation_mode` selects `scalar` or `ascii_blocks`; default is `ascii_blocks`. The scalar template has the original per-character loop, so the comparison does not charge a per-character mode branch only to the reference. This is a developer diagnostic control, not a new SQL/JSONL option. The production Docker entrance and protocol remain unchanged.

Independent byte-range UTF8 oracle tests cover both modes, character limits, embedded NUL and DEL, Unicode bounds, random valid/invalid bytes, unaligned addresses, lengths around 8-byte boundaries and inaccessible guard pages. Corrupted text after 8 or more ASCII characters must still be rejected when unprojected or filtered out. The targeted suite reports 321,404 comparisons per mode. These are behavior-preserving tests that already passed on the original scalar implementation, not an invented functional red/green bug fix.

## Same-binary measurements

Default 8 MiB scan snapshot, paired traversal, compact pages and 16 MiB data cache remain enabled in both modes. Each sample uses a fresh database; result digests, row/distance counts, file sizes and complete reopened contents match. The core harness generates/checks truth separately from timed queries, but its memory belongs to the measured container, so this is not official database-only M. Three alternating rounds initially cover 30 samples and 2,160 queries.

| Workload | Scalar median ms | ASCII blocks median ms | Change in query time |
| --- | ---: | ---: | ---: |
| 10,000 rows, 128 dimensions, 200 queries | 647.429 | 581.471 | -10.19% |
| Mixed Unicode text, 2,000 rows, 40 queries | 38.961 | 37.740 | -3.13% |
| 768 dimensions, 2,000 rows, 40 queries | 100.755 | 110.743 | **+9.91%** |
| 5,000-character text exceeding snapshot capacity | 435.241 | 353.260 | -18.84% |
| Update before every query | 83.095 | 74.402 | -10.46% |

The 768-dimensional regression triggered a seven-round repeat, with the exact same binary and source: 107.637 -> 101.796 ms (-5.43%). We keep both results; this reversal does not prove the initial regression was a specific environmental fault or guarantee improvement for this workload. Initial dense write time was 1343.451 -> 1357.835 ms (+1.07%); this small regression is retained. Mixed Unicode is approximately neutral. Dense core cgroup peak was 84,377,600 -> 84,832,256 B, a small observed increase, despite no new heap allocation in the algorithm.

Two additional one-round instrumented probes are diagnostics only. In the 2,000-row dense probe, row-validation time fell from 7.653 to 6.412 ms while total measured time rose from 18.877 to 19.820 ms. Do not equate an improved substage with guaranteed total improvement. Across uninstrumented initial and repeat controls: **44 samples, 2,720 main queries, 44 first-query checks and 44 complete reopens**. Instrumented probes add four samples and 120 main queries, reported separately.

## Final Docker verification and actual JSONL

Final runtime image: `sha256:463556d0233119a7bec1956179ddba76ce61825c135308e5bebd1d4dbb5ec563`. **31/31 internal tests passed in 155.27 seconds; 20/20 official public cases (95 SQL assertions), 273 independent model checks with 150 mutations and two kill/restarts, and the default JSONL runtime/persistence/rollback smoke checks passed.**

Actual default runtime, 10,000 rows x 128 dimensions, 1,000 queries per sample, three rounds with indexed/unindexed order alternated:

| Median | Uploaded 1349 baseline | Current text optimization |
| --- | ---: | ---: |
| Indexed queries | 4956.946 ms | 4061.655 ms |
| Unindexed queries | 7302.234 ms | 5924.890 ms |
| Indexed load | 1912.988 ms | 1786.046 ms |
| Index construction | 75.491 ms | 69.751 ms |
| Container peak through queries | 72,740,864 B | 72,626,176 B |
| Final indexed database size | 42,778,624 B | 42,778,624 B |
| Cumulative physical read/write bytes through queries | 43,196,416 B | 43,196,416 B |

All 6,000 timed queries, 60 warmups, 24 extra ANN checks and six restarts passed. Input hash is unchanged: `9c7a1ae137870b946a9251c2c6549f2c17e723dad1da29898e4c2660713f5424`. Indexed query time is 18.06% lower than the prior uploaded-version measurement. These old/new images were measured in separate phases; environmental variation may contribute to the difference, so the same-binary controls above are the stronger evidence for the isolated text-validation effect. Default retained for the measured common-case and long-text benefits, with the original negative wide-vector data preserved.

## Learning evidence

The mirrored conversation JSON contains the actual user-confirmed failed SHA, separate old/new reproduction results and a version context explaining historical statements. `review_navigation` points to authentic student requests and separate result strings within that same JSON, with exact excerpts no longer than 240 characters. It does not assign grades, duplicate one chain into multiple units, attribute forwarded organizer text to student analysis, or turn agent explanations into student understanding. The material can become easier to assess without any promise of a higher score.

## Reproduction and limitations

Compile `bench/paired.cpp` against the current core library as `build/text-bench`; use `bench/run_text_validation.py --output <new D-drive path> --rounds 3 --suite dense` and `--suite varied`. Use `--suite varied --case wide --rounds 7` for the repeat and `--suite profile` only for instrumented diagnostics. `MVDB_TEXT_SCALAR=1` selects the reference in the core harness; normal runtime uses the default. All evidence is under `bench/results/text-validation-20261110/manifest.json`.

This is a local warm-cache experiment, not the frozen official benchmark. WSL does not enforce the requested no-swap limit. No formal E or ranking score is claimed. Published public cases do not establish hidden formal admission; the platform has not supplied the failure category/logs for 0e954.
