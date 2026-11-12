#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
if [ -f work/unindexed-red.log ] && [ ! -f work/unindexed-test-fixture-error.log ]; then
  cp work/unindexed-red.log work/unindexed-test-fixture-error.log
fi
cmake -S repo -B build/unindexed-baseline-core -G Ninja -DCMAKE_BUILD_TYPE=Release > work/unindexed-red-configure.log 2>&1
cmake --build build/unindexed-baseline-core --target unindexed_snapshot_tests --parallel 2 > work/unindexed-red-build.log 2>&1
set +e
MVDB_TEST_DIR=/mnt/d/Codex/MiniVectorDB/data/unindexed-red build/unindexed-baseline-core/unindexed_snapshot_tests > work/unindexed-red.log 2>&1
result=$?
set -e
cat work/unindexed-red.log
test "$result" -eq 1
python3 -c "from pathlib import Path; assert 'warm.info.snapshot_rows_examined' in Path('work/unindexed-red.log').read_text()"
printf 'Expected baseline test failure observed.\n'
