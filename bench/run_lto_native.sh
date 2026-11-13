#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
result="$PWD/lto-results"
mkdir -p "$result" "$RUNNER_TEMP/mvdb-native-data" "$RUNNER_TEMP/mvdb-native-tmp"
git rev-parse HEAD > "$result/source-commit.txt"
lscpu | tee "$result/cpu.txt"
uname -a > "$result/kernel.txt"
docker build --tag mvdb:lto-reference . > "$result/build-reference.log" 2>&1
python3 - <<'PY'
from pathlib import Path
s=Path('Dockerfile').read_text()
assert s.count('-DCMAKE_BUILD_TYPE=Release')==1
Path('lto-results/Dockerfile.lto').write_text(s.replace('-DCMAKE_BUILD_TYPE=Release','-DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON'))
PY
docker build -f "$result/Dockerfile.lto" --target test --tag mvdb:lto-test . > "$result/build-test.log" 2>&1
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g mvdb:lto-test 2>&1 | tee "$result/ctest.log"
docker build -f "$result/Dockerfile.lto" --tag mvdb:lto-candidate . > "$result/build-runtime.log" 2>&1
reference=$(docker image inspect --format '{{.Id}}' mvdb:lto-reference)
candidate=$(docker image inspect --format '{{.Id}}' mvdb:lto-candidate)
printf '%s\n' "$reference" "$candidate" > "$result/images.txt"
python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image "$candidate" --output "$result/public"
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$RUNNER_TEMP/mvdb-native-oracle"
for filter in bigint varchar; do
  sudo python3 -B bench/run_linux_contract.py --reference-image "$reference" --candidate-image "$candidate" \
    --oracle-bin "$RUNNER_TEMP/mvdb-native-oracle" --output "$result/$filter" \
    --data-root "$RUNNER_TEMP/mvdb-native-data" --temp-root "$RUNNER_TEMP/mvdb-native-tmp" \
    --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$filter"
done
sudo python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);results={}
for f in ['bigint','varchar']:
    d=json.loads((p/f/'summary.json').read_text())
    results[f]={'metadata':d['metadata'],'groups':{k:{'median':g['median'],'samples':[{q:s[q] for q in ['round','T_ms','B_bytes','M_bytes','passed','swap_bytes','full_rows_before_restart','full_rows_after_restart','restart_query_checked'] if q in s} for s in g['samples']]} for k,g in d['groups'].items()}}
(p/'comparison.json').write_text(json.dumps(results,indent=2)+'\n')
print('LTO_CONTRACT_RESULT '+json.dumps(results))
PY
sudo chown -R "$(id -u):$(id -g)" "$result"
