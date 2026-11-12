#!/usr/bin/env bash
# Relative native x86_64 measurements; these are not competition E scores.
set -euo pipefail
test "$(uname -m)" = x86_64
baseline=aea69813bb6cfb1b0abea3d892db0d4c32517829
base_dir="$RUNNER_TEMP/mvdb-frozen-base"
result="$PWD/native-results"
mkdir -p "$base_dir" "$result" "$RUNNER_TEMP/mvdb-native-data" "$RUNNER_TEMP/mvdb-native-tmp"
git -C "$base_dir" init
git -C "$base_dir" remote add origin https://github.com/juanzhi-gcb/seekdb.git
git -C "$base_dir" fetch --depth=1 origin "$baseline"
git -C "$base_dir" checkout --detach FETCH_HEAD
test "$(git -C "$base_dir" rev-parse HEAD)" = "$baseline"
git rev-parse HEAD > "$result/candidate-commit.txt"
printf '%s\n' "$baseline" > "$result/reference-commit.txt"
uname -a > "$result/kernel.txt"
lscpu > "$result/cpu.txt"
docker info > "$result/docker-info.txt"
docker build --tag mvdb:native-reference "$base_dir" 2>&1 | tee "$result/build-reference.log"
docker build --target test --tag mvdb:native-test . 2>&1 | tee "$result/build-test.log"
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g mvdb:native-test 2>&1 | tee "$result/ctest.log"
docker build --tag mvdb:native-candidate . 2>&1 | tee "$result/build-runtime.log"
reference=$(docker image inspect --format '{{.Id}}' mvdb:native-reference)
candidate=$(docker image inspect --format '{{.Id}}' mvdb:native-candidate)
printf '%s\n' "$reference" "$candidate" > "$result/image-digests.txt"
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
root=Path(sys.argv[1]); result={}
for f in ['bigint','varchar']:
    data=json.loads((root/f/'summary.json').read_text())
    groups={}
    for name,g in data['groups'].items():
        samples=[]
        for s in g['samples']:
            samples.append({k:s[k] for k in ['round','T_ms','B_bytes','M_bytes','passed','swap_bytes','full_rows_before_restart','full_rows_after_restart','restart_query_checked'] if k in s})
        groups[name]={'median':g['median'],'samples':samples}
    result[f]={'metadata':data['metadata'],'groups':groups}
(root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print('NATIVE_CONTRACT_RESULT '+json.dumps(result))
PY
sudo chown -R "$(id -u):$(id -g)" "$result"
