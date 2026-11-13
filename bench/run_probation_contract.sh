#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
repo=$(git rev-parse --show-toplevel)
cd "$repo"
result="$repo/probation-results"
test ! -e "$result"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-probation-XXXXXX")
mkdir "$scratch/baseline" "$scratch/previous" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
baseline=ff117199f8077de7a6e3e1f91a611397bb6641a0
previous=b170c05f3138a08f5c662f15c932bafc661901ab
git archive "$baseline" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/baseline"
git archive "$previous" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/previous"
git rev-parse HEAD > "$result/candidate-commit.txt"
printf '%s\n' "$baseline" > "$result/baseline-commit.txt"
printf '%s\n' "$previous" > "$result/previous-commit.txt"
lscpu > "$result/lscpu.txt"
uname -a > "$result/kernel.txt"
limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges)
docker build -t mvdb:probation-base "$scratch/baseline" > "$result/build-base.log" 2>&1
docker build -t mvdb:probation-previous "$scratch/previous" > "$result/build-previous.log" 2>&1
docker build --target test -t mvdb:probation-test . > "$result/build-test.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" --entrypoint ctest mvdb:probation-test --test-dir /build -V > "$result/ctest.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" --entrypoint /build/bounded_screen_tests mvdb:probation-test > "$result/bounds.json" 2> "$result/bounds.stderr.log"
docker build --target test -t mvdb:probation-previous-test "$scratch/previous" > "$result/build-previous-test.log" 2>&1
mkdir -p "$scratch/probe/off" "$scratch/probe/on"
chmod -R 0777 "$scratch/probe"
for mode in off on; do
  probe_image=mvdb:probation-test;[[ "$mode" != off ]] || probe_image=mvdb:probation-previous-test
  docker run --rm "${limits[@]}" --mount "type=bind,src=$repo/bench/probation_probe.cpp,dst=/probe.cpp,readonly" \
    --mount "type=bind,src=$scratch/probe/$mode,dst=/out" --entrypoint sh "$probe_image" \
    -c 'g++ -std=c++20 -O3 -ffp-contract=off -I/src/include -I/src/src /probe.cpp /build/libminivectordb_core.a -o /out/probe' > "$result/probe-build-$mode.log" 2>&1
done
for round in 1 2 3; do
  for mode in off on; do
    probe_image=mvdb:probation-test;[[ "$mode" != off ]] || probe_image=mvdb:probation-previous-test
    docker run --rm "${limits[@]}" -e MVDB_TEXT_FILTER=1 --mount "type=bind,src=$scratch/probe/$mode,dst=/out,readonly" \
      --mount "type=bind,src=$scratch/data,dst=/data" --entrypoint /out/probe "$probe_image" \
      "/data/probe-$round-$mode.db" 10000 200 1 1 > "$result/probe-$round-$mode.json" 2> "$result/probe-$round-$mode.stderr.log"
  done
done
docker build -t mvdb:probation-candidate . > "$result/build-runtime.log" 2>&1
base=$(docker image inspect --format '{{.Id}}' mvdb:probation-base)
previous_image=$(docker image inspect --format '{{.Id}}' mvdb:probation-previous)
candidate=$(docker image inspect --format '{{.Id}}' mvdb:probation-candidate)
printf '%s\n' "$base" "$previous_image" "$candidate" > "$result/image-identities.txt"
python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image "$candidate" --output "$result/public" > "$result/public.log" 2>&1
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for schema in bigint varchar; do
  for comparison in formal previous; do
    reference="$base";[[ "$comparison" != previous ]] || reference="$previous_image"
    sudo python3 -B bench/run_linux_contract.py --reference-image "$reference" --candidate-image "$candidate" \
      --oracle-bin "$scratch/oracle" --output "$result/$comparison-$schema" --data-root "$scratch/data/contract" \
      --temp-root "$scratch/tmp/contract" --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$schema" \
      > "$result/$comparison-$schema.log" 2>&1
  done
  sudo python3 -B bench/run_general_contract.py --reference-image "$base" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/general-$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 1 --rows 10000 --queries 200 --warmup 10 --filter-type "$schema" \
    > "$result/general-$schema.log" 2>&1
  sudo python3 -B bench/run_linux_contract.py --reference-image "$base" --candidate-image "$candidate" \
    --oracle-bin "$scratch/oracle" --output "$result/capacity-$schema" --data-root "$scratch/data/contract" \
    --temp-root "$scratch/tmp/contract" --rounds 1 --rows 30000 --queries 100 --warmup 10 --filter-type "$schema" \
    > "$result/capacity-$schema.log" 2>&1
done
sudo python3 - "$result" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);out={'scope':'Local generated datasets; no official E conversion','comparisons':{}}
for p in sorted(root.glob('*/summary.json')):
    d=json.loads(p.read_text());ratios={}
    for phase in ['write/unindexed','query/unindexed','query/indexed']:
        a=d['groups']['reference/'+phase]['median'];b=d['groups']['candidate/'+phase]['median']
        ratios[phase]={k:b[k]/a[k] for k in ['T_ms','B_bytes','M_bytes']}
    out['comparisons'][p.parent.name]={'ratios':ratios,'groups':d['groups'],'metadata':d['metadata']}
(root/'comparison.json').write_text(json.dumps(out,indent=2)+'\n')
print('INT8SCREEN_COMPARISON '+json.dumps({k:v['ratios'] for k,v in out['comparisons'].items()}))
for path in root.rglob('oracle-input.bin'):path.unlink()
PY
