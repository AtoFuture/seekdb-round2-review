#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
g++ -std=c++20 -O2 -I repo/include repo/tests/typed_scan_tests.cpp build/unindexed-candidate-core/libminivectordb_core.a -o build/typed-red-tests
set +e
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/typed-red build/typed-red-tests > work/typed-red.log 2>&1
status=$?
set -e
cat work/typed-red.log
printf 'baseline_exit=%s\n' "$status"
test "$status" -ne 0