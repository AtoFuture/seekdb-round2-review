#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
cmake -S repo -B build/unindexed-candidate-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/unindexed-v3-configure.log 2>&1
cmake --build build/unindexed-candidate-core --target minivectordb unindexed_snapshot_tests scan_snapshot_tests selective_tests numeric_sql_tests row_validation_tests profile_tests data_cache_sql_tests vector_scratch_tests --parallel 2 > work/unindexed-v3-build.log 2>&1
set +e
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/vector-scratch-red build/unindexed-candidate-core/vector_scratch_tests > work/vector-scratch-red.log 2>&1
status=$?
set -e
cat work/vector-scratch-red.log
test "$status" -eq 1
grep -q 'vector_values_materialized==0' work/vector-scratch-red.log
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/unindexed-v3 ctest --test-dir build/unindexed-candidate-core -R 'core_(unindexed_snapshot|scan_snapshot|selective|numeric_ties|numeric_maximum|row_validation|profile|data_cache_sql)$' --output-on-failure > work/unindexed-v3-tests.log 2>&1
tail -6 work/unindexed-v3-tests.log
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/paired.cpp build/unindexed-candidate-core/libminivectordb_core.a -o build/unindexed-v3-bench
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v3-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v3-short.jsonl --rounds 5 --suite varied --case short_reuse --modes reference,release,reuse
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v3-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v3-interleaved.jsonl --rounds 5 --suite varied --case interleaved --modes reference,release,reuse
