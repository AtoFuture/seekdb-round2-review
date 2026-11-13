#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
cmake -S repo -B build/score-first-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/score-kernel-configure.log 2>&1
cmake --build build/score-first-core --parallel 2 > work/score-kernel-build.log 2>&1
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/score-kernel ctest --test-dir build/score-first-core -R 'core_(validated_vector|typed_scan|commit_policy|numeric_ties|numeric_maximum)$' --output-on-failure > work/score-kernel-tests.log 2>&1
tail -10 work/score-kernel-tests.log
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/typed.cpp build/score-first-core/libminivectordb_core.a -o build/score-kernel-bench
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-kernel-bench --output /mnt/d/Codex/MiniVectorDB/work/score-kernel-dense.jsonl --rounds 6 --modes typed_4m,optimized
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-kernel-bench --output /mnt/d/Codex/MiniVectorDB/work/score-kernel-varied.jsonl --suite varied --rounds 3 --modes typed_4m,optimized
