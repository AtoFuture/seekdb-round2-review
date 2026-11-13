#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
set +e
g++ -std=c++20 -O2 -I repo/include repo/tests/validated_vector_tests.cpp build/score-first-core/libminivectordb_core.a -o build/validated-vector-red > work/validated-vector-red.log 2>&1
status=$?
set -e
cat work/validated-vector-red.log
printf 'baseline_exit=%s\n' "$status"
test "$status" -ne 0