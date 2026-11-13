#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
cmake -S repo -B build/unindexed-baseline-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/unindexed-baseline-configure.log 2>&1
cmake --build build/unindexed-baseline-core --target minivectordb profile_tests --parallel 2 > work/unindexed-baseline-build.log 2>&1
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/unindexed-baseline ctest --test-dir build/unindexed-baseline-core -R '^core_profile$' --output-on-failure > work/unindexed-baseline-profile-tests.log 2>&1
tail -5 work/unindexed-baseline-profile-tests.log
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/paired.cpp build/unindexed-baseline-core/libminivectordb_core.a -o build/unindexed-baseline-bench
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-baseline-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-baseline-timed.jsonl --rounds 3
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-baseline-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-baseline-profile.jsonl --rounds 2 --profile
