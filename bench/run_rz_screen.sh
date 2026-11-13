#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
repo=$(git rev-parse --show-toplevel)
cd "$repo"
result="$repo/rz-screen-results"
test ! -e "$result"
mkdir "$result"
scratch=$(mktemp -d "$RUNNER_TEMP/mvdb-rz-XXXXXX")
mkdir -p "$scratch/baseline" "$scratch/bin/base" "$scratch/bin/rz" "$scratch/data" "$scratch/tmp"
chmod 0777 "$scratch/bin/base" "$scratch/bin/rz" "$scratch/data" "$scratch/tmp"
export TMPDIR="$scratch/tmp" PYTHONDONTWRITEBYTECODE=1
trap 'sudo chown -R "$(id -u):$(id -g)" "$result"' EXIT
git rev-parse HEAD > "$result/candidate-commit.txt"
baseline=ff117199f8077de7a6e3e1f91a611397bb6641a0
printf '%s\n' "$baseline" > "$result/baseline-commit.txt"
git archive "$baseline" Dockerfile CMakeLists.txt src include tests | tar -x -C "$scratch/baseline"
lscpu > "$result/lscpu.txt"
uname -a > "$result/kernel.txt"
docker version > "$result/docker-version.txt"
docker build --target test -t mvdb:rz-reference "$scratch/baseline" > "$result/build-reference.log" 2>&1
docker build --target test -t mvdb:rz-candidate . > "$result/build-candidate.log" 2>&1
limits=(--network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 --cap-drop ALL --security-opt no-new-privileges)
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" mvdb:rz-candidate > "$result/ctest.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$scratch/data,dst=/data" \
  --entrypoint /build/bounded_screen_tests mvdb:rz-candidate > "$result/bounds.json" 2> "$result/bounds.stderr.log"
docker run --rm "${limits[@]}" --mount "type=bind,src=$repo/bench,dst=/bench,readonly" \
  --mount "type=bind,src=$scratch/bin/base,dst=/out" --entrypoint sh mvdb:rz-reference -c \
  'g++ -std=c++20 -O3 -ffp-contract=off -I/src/include /bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/probe' > "$result/compile-base.log" 2>&1
docker run --rm "${limits[@]}" --mount "type=bind,src=$repo/bench,dst=/bench,readonly" \
  --mount "type=bind,src=$scratch/bin/rz,dst=/out" --entrypoint sh mvdb:rz-candidate -c \
  'g++ -std=c++20 -O3 -ffp-contract=off -I/src/include /bench/rz_screen_diagnostic.cpp /build/libminivectordb_core.a -o /out/probe' > "$result/compile-rz.log" 2>&1
docker run --rm --network none --entrypoint objdump mvdb:rz-candidate -d -C /build/libminivectordb_core.a > "$result/core-disassembly.txt"
python3 -B - "$result/core-disassembly.txt" <<'PY'
import re,sys
text=open(sys.argv[1]).read()
match=re.search(r'<minivectordb::detail::batch16_lower_bound_avx512\([^\n]*\)>:\n(.*?)(?=\n\n)',text,re.S)
assert match, 'missing lower-bound kernel disassembly'
body=match.group(1)
for op in ['vsubps','vfmadd','vaddps']:
    assert re.search(op+r'[^\n]*\{rz-sae\}',body), (op,body)
assert re.search(r'vfmadd[^\n]*\{rz-sae\}',body)
print('Verified explicit AVX512 RZ subtraction/FMA/reduction; strict returned-distance kernel unchanged')
PY
sha256sum bench/rz_screen_diagnostic.cpp bench/score_kernel_diagnostic.cpp > "$result/probe-source.sha256"
for kind in base rz; do sudo sha256sum "$scratch/bin/$kind/probe" >> "$result/probe-binaries.sha256"; done
mkdir "$result/micro"
for round in 1 2 3; do
  order=(baseline off on)
  if [[ "$round" = 2 ]]; then order=(on off baseline); fi
  if [[ "$round" = 3 ]]; then order=(off baseline on); fi
  if [[ "${NATIVE_REPLICA:-1}" = 2 ]]; then order=("${order[@]:1}" "${order[@]:0:1}"); fi
  for schema in bigint varchar; do
    for variant in "${order[@]}"; do
      args=(--env MVDB_BENCH_CGROUP=1)
      [[ "$schema" != varchar ]] || args+=(--env MVDB_TEXT_FILTER=1)
      [[ "$variant" != off ]] || args+=(--env MVDB_RZ_OFF=1)
      kind=rz;image=mvdb:rz-candidate
      if [[ "$variant" = baseline ]]; then kind=base;image=mvdb:rz-reference; fi
      label="r$round-$schema-$variant"
      printf '%s\n' "$label" >> "$result/micro/order.txt"
      docker run --rm "${limits[@]}" "${args[@]}" \
        --mount "type=bind,src=$scratch/bin/$kind,dst=/probe,readonly" \
        --mount "type=bind,src=$scratch/data,dst=/data" --entrypoint /probe/probe "$image" \
        "/data/$label/db" 10000 1000 1 1 > "$result/micro/$label.json" 2> "$result/micro/$label.stderr.log"
    done
  done
done
python3 -B - "$result" <<'PY'
import json,statistics,sys
from pathlib import Path
root=Path(sys.argv[1]);out={'scope':'diagnostic only; no official E estimate','bounds':json.loads((root/'bounds.json').read_text()),'micro':{}}
for schema in ['bigint','varchar']:
    samples={v:[json.loads((root/'micro'/f'r{r}-{schema}-{v}.json').read_text()) for r in [1,2,3]] for v in ['baseline','off','on']}
    digest=samples['baseline'][0]['result_digest']; group={}
    for v,rows in samples.items():
        assert all(x['validated_queries']==1000 and x['recovered_rows']==10000 and x['result_digest']==digest and x['profile_enabled'] is False for x in rows)
        group[v]={'samples':rows,'median':{k:statistics.median(x[k] for x in rows) for k in ['write_ms','query_total_ms','query_filtered_ms','query_unfiltered_ms','database_bytes','process_maxrss_kib','local_cgroup_peak_bytes']}}
    for v in ['off','on']:
        group[v]['ratio_to_formal_baseline']={k:x/group['baseline']['median'][k] for k,x in group[v]['median'].items()}
    out['micro'][schema]=group
(root/'summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({schema:out['micro'][schema]['on']['ratio_to_formal_baseline'] for schema in out['micro']}))
PY
