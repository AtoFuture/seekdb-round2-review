#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
cmake -S repo -B build/unindexed-candidate-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/unindexed-v4-configure.log 2>&1
cmake --build build/unindexed-candidate-core --target minivectordb unindexed_snapshot_tests scan_snapshot_tests selective_tests numeric_sql_tests row_validation_tests row_allocation_tests profile_tests data_cache_sql_tests vector_scratch_tests --parallel 2 > work/unindexed-v4-build.log 2>&1
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/unindexed-v4 ctest --test-dir build/unindexed-candidate-core -R 'core_(unindexed_snapshot|scan_snapshot|selective|numeric_ties|numeric_maximum|row_validation|row_allocation|profile|data_cache_sql|vector_scratch)$' --output-on-failure > work/unindexed-v4-tests.log 2>&1
tail -6 work/unindexed-v4-tests.log
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/paired.cpp build/unindexed-candidate-core/libminivectordb_core.a -o build/unindexed-v4-bench
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v4-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v4-dense.jsonl --rounds 6 --modes reference,reuse,scratch
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v4-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v4-varied.jsonl --rounds 3 --suite varied --modes reference,scratch
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v4-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v4-short.jsonl --rounds 6 --suite varied --case short_reuse --modes reference,release,reuse
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v4-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v4-indexed.jsonl --rounds 3 --modes indexed_reference,indexed
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v4-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v4-profile.jsonl --rounds 2 --profile --modes reference,scratch
