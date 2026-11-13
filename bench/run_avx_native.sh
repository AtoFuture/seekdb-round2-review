#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
bin="$RUNNER_TEMP/mvdb-avx-bin"
data="$RUNNER_TEMP/mvdb-avx-data"
mkdir -p "$bin" "$data" avx-results
lscpu | tee avx-results/cpu.txt
docker build --target test --tag mvdb:avx-native . 2>&1 | tee avx-results/build.log
docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:avx-native -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/reference'
for variant in avx2 avx2_transposed; do
  docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:avx-native -c "g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_${variant}_probe.cpp /candidate/tests/validated_vector_tests.cpp -o /out/check && /out/check && g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_${variant}_probe.cpp /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/${variant}" 2>&1 | tee "avx-results/check-${variant}.log"
done
for round in 1 2 3; do
  order='reference avx2 avx2_transposed'
  if [[ "$round" = 2 ]]; then order='avx2_transposed avx2 reference'; fi
  for binary in $order; do
    for filter in bigint varchar; do
      run="r$round-$filter-$binary"
      args=()
      if [[ "$filter" = varchar ]]; then args+=(-e MVDB_TEXT_FILTER=1); fi
      docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g "${args[@]}" -v "$bin:/probe:ro" -v "$data:/data" --entrypoint "/probe/$binary" mvdb:avx-native "/data/$run/db" 10000 1000 1 1 > "avx-results/$run.json"
      echo "RESULT $run"
      cat "avx-results/$run.json"
    done
  done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
p=Path('avx-results')
for filter in ['bigint','varchar']:
    base=None
    for variant in ['reference','avx2','avx2_transposed']:
        ds=[json.loads((p/f'r{r}-{filter}-{variant}.json').read_text()) for r in [1,2,3]]
        for r,d in zip([1,2,3],ds):
            assert d['result_digest']==json.loads((p/f'r{r}-{filter}-reference.json').read_text())['result_digest']
        med=statistics.median(d['query_total_ms'] for d in ds)
        if base is None:base=med
        print('AVX_SUMMARY '+json.dumps({'filter':filter,'variant':variant,'query_ms':[d['query_total_ms'] for d in ds],'median_ms':med,'relative_percent':(med/base-1)*100,'exact_results_agree':True}))
PY
