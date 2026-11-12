#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
cmake -S repo -B build/score-first-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/score-first-configure.log 2>&1
cmake --build build/score-first-core --target minivectordb typed_scan_tests sql_tests vector_index_tests numeric_sql_tests unindexed_snapshot_tests --parallel 2 > work/score-first-build.log 2>&1
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/score-first-quick ctest --test-dir build/score-first-core -R 'core_(typed_scan|sql|sql_edges|sql_failure|sql_batch|vector_index|numeric_maximum|numeric_ties|unindexed_snapshot)$' --output-on-failure > work/score-first-quick.log 2>&1
tail -7 work/score-first-quick.log
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/typed.cpp build/score-first-core/libminivectordb_core.a -o build/score-first-bench
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-first-bench --output /mnt/d/Codex/MiniVectorDB/work/typed-first-dense.jsonl --rounds 2 --modes reference,typed