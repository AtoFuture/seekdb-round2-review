#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
cmake --build build/unindexed-candidate-core --target minivectordb unindexed_snapshot_tests scan_snapshot_tests selective_tests numeric_sql_tests row_validation_tests profile_tests data_cache_sql_tests --parallel 2 > work/unindexed-v2-build.log 2>&1
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/unindexed-v2 ctest --test-dir build/unindexed-candidate-core -R 'core_(unindexed_snapshot|scan_snapshot|selective|numeric_ties|numeric_maximum|row_validation|profile|data_cache_sql)$' --output-on-failure > work/unindexed-v2-tests.log 2>&1
tail -6 work/unindexed-v2-tests.log
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/paired.cpp build/unindexed-candidate-core/libminivectordb_core.a -o build/unindexed-v2-bench
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v2-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v2-varied.jsonl --rounds 3 --suite varied --modes reference,snapshot
