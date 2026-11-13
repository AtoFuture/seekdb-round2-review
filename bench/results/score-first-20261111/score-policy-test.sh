#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
cmake -S repo -B build/score-first-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/score-policy-configure.log 2>&1
cmake --build build/score-first-core --parallel 2 > work/score-policy-build.log 2>&1
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/score-policy ctest --test-dir build/score-first-core -R 'core_(typed_scan|commit_policy|scan_snapshot|unindexed_snapshot|vector_scratch|profile|selective|sql_crash|sql_failure|numeric_ties|numeric_maximum)$' --output-on-failure > work/score-policy-tests.log 2>&1
tail -8 work/score-policy-tests.log