#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
cmake --build build/unindexed-candidate-core --target unindexed_snapshot_tests --parallel 2 > work/unindexed-arena-red-build.log 2>&1
set +e
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/unindexed-arena-red build/unindexed-candidate-core/unindexed_snapshot_tests > work/unindexed-arena-red.log 2>&1
status=$?
set -e
cat work/unindexed-arena-red.log
test "$status" -eq 1
grep -q 'cold.info.snapshot_bytes==4096' work/unindexed-arena-red.log
