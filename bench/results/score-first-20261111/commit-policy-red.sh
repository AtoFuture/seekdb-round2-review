#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
g++ -std=c++20 -O2 -I repo/include repo/tests/commit_policy_tests.cpp build/unindexed-candidate-core/libminivectordb_core.a -Wl,--wrap=pwrite -Wl,--wrap=fsync -o build/commit-policy-red
set +e
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/commit-policy-red build/commit-policy-red --red > work/commit-policy-red.log 2>&1
status=$?
set -e
cat work/commit-policy-red.log
printf 'baseline_exit=%s\n' "$status"
test "$status" -ne 0