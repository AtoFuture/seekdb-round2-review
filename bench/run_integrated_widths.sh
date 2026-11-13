#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
bin="$RUNNER_TEMP/mvdb-width-bin"
data="$RUNNER_TEMP/mvdb-width-data"
mkdir -p "$bin" "$data" width-results
lscpu | tee width-results/cpu.txt
docker build --target test --tag mvdb:width-native . > width-results/build.log 2>&1
docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:width-native -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/batch16'
for width in 4 8; do
  docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:width-native -c "g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_integrated${width}_probe.cpp /candidate/tests/validated_vector_tests.cpp -o /out/check${width} && /out/check${width} && g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_integrated${width}_probe.cpp /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/batch${width}" 2>&1 | tee "width-results/check${width}.log"
done
for round in 1 2 3; do
  order='scalar batch4 batch8 batch16'
  if [[ "$round" = 2 ]]; then order='batch16 batch8 batch4 scalar'; fi
  for variant in $order; do
    for filter in bigint varchar; do
      run="r$round-$filter-$variant"
      args=()
      binary="$variant"
      if [[ "$variant" = scalar ]]; then args+=(-e MVDB_SCALAR_KERNEL=1); binary=batch16; fi
      if [[ "$filter" = varchar ]]; then args+=(-e MVDB_TEXT_FILTER=1); fi
      docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g "${args[@]}" -v "$bin:/probe:ro" -v "$data:/data" --entrypoint "/probe/$binary" mvdb:width-native "/data/$run/db" 10000 1000 1 1 > "width-results/$run.json"
    done
  done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
p=Path('width-results')
for filter in ['bigint','varchar']:
    base=None
    for variant in ['scalar','batch4','batch8','batch16']:
        ds=[json.loads((p/f'r{r}-{filter}-{variant}.json').read_text()) for r in [1,2,3]]
        for r,d in zip([1,2,3],ds):
            assert d['result_digest']==json.loads((p/f'r{r}-{filter}-scalar.json').read_text())['result_digest']
        med=statistics.median(d['query_total_ms'] for d in ds)
        if base is None:base=med
        print('WIDTH_SUMMARY '+json.dumps({'filter':filter,'variant':variant,'query_ms':[d['query_total_ms'] for d in ds],'median_ms':med,'relative_percent':(med/base-1)*100,'exact_results_agree':True}))
PY
