#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
repo=$(git rev-parse --show-toplevel)
cd "$repo"
result="$repo/bundle-results"
test ! -e "$result"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-bundle-XXXXXX")
mkdir "$scratch/baseline" "$scratch/hint" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
baseline=ff117199f8077de7a6e3e1f91a611397bb6641a0
git archive "$baseline" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/baseline"
git archive HEAD Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/hint"
python3 - "$scratch/hint/include/minivectordb/database.hpp" <<'PY'
import sys
from pathlib import Path
p=Path(sys.argv[1]);s=p.read_text();old='bool discard_file_cache = false;'
assert s.count(old)==1
p.write_text(s.replace(old,'bool discard_file_cache = true;'))
PY
git diff --no-index include/minivectordb/database.hpp "$scratch/hint/include/minivectordb/database.hpp" > "$result/advisory-option.patch" || test "$?" = 1
git rev-parse HEAD > "$result/candidate-commit.txt"
printf '%s\n' "$baseline" > "$result/baseline-commit.txt"
lscpu > "$result/lscpu.txt"
uname -a > "$result/kernel.txt"
limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges)
docker build -t mvdb:bundle-base "$scratch/baseline" > "$result/build-base.log" 2>&1
for variant in combined hint; do
  src=.;[[ "$variant" != hint ]] || src="$scratch/hint"
  docker build --target test -t "mvdb:bundle-$variant-test" "$src" > "$result/build-$variant-test.log" 2>&1
  docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" "mvdb:bundle-$variant-test" > "$result/ctest-$variant.log" 2>&1
  docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" --entrypoint /build/bounded_screen_tests "mvdb:bundle-$variant-test" > "$result/bounds-$variant.json" 2> "$result/bounds-$variant.stderr.log"
  docker build -t "mvdb:bundle-$variant" "$src" > "$result/build-$variant-runtime.log" 2>&1
  candidate=$(docker image inspect --format '{{.Id}}' "mvdb:bundle-$variant")
  printf '%s %s\n' "$variant" "$candidate" >> "$result/image-identities.txt"
  python3 -B bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image "$candidate" --output "$result/public-$variant" > "$result/public-$variant.log" 2>&1
done
base=$(docker image inspect --format '{{.Id}}' mvdb:bundle-base)
combined=$(docker image inspect --format '{{.Id}}' mvdb:bundle-combined)
hint=$(docker image inspect --format '{{.Id}}' mvdb:bundle-hint)
printf 'baseline %s\n' "$base" >> "$result/image-identities.txt"
g++ -std=c++20 -O2 -ffp-contract=off bench/jsonl_oracle.cpp -o "$scratch/oracle"
for schema in bigint varchar; do
  for comparison in formal advisory; do
    reference="$base";candidate="$combined"
    if [[ "$comparison" = advisory ]]; then reference="$combined";candidate="$hint"; fi
    sudo python3 -B bench/run_linux_contract.py --reference-image "$reference" --candidate-image "$candidate" \
      --oracle-bin "$scratch/oracle" --output "$result/$comparison-$schema" --data-root "$scratch/data/contract" \
      --temp-root "$scratch/tmp/contract" --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$schema" \
      > "$result/$comparison-$schema.log" 2>&1
  done
  sudo python3 -B bench/run_linux_contract.py --reference-image "$base" --candidate-image "$combined" \
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
print('BUNDLE_COMPARISON '+json.dumps({k:v['ratios'] for k,v in out['comparisons'].items()}))
# These inputs can be reconstructed exactly from the retained seed and source.
# Keep all measurements, oracle outputs, diagnostics and failures.
for path in root.rglob('oracle-input.bin'):path.unlink()
PY
