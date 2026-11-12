#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
bin="$RUNNER_TEMP/mvdb-layout-bin"
data="$RUNNER_TEMP/mvdb-layout-data"
mkdir -p "$bin" "$data" layout-results
docker build --target test --tag mvdb:layout-native . 2>&1 | tee layout-results/build.log
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:layout-native -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_transposed_probe.cpp /candidate/tests/validated_vector_tests.cpp -o /out/check && /out/check && g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_diagnostic.cpp /build/libminivectordb_core.a -o /out/reference && g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_transposed_probe.cpp /candidate/bench/score_diagnostic.cpp /build/libminivectordb_core.a -o /out/transposed' 2>&1 | tee layout-results/compile-and-check.log
for round in 1 2 3; do
  order='reference transposed'
  if [[ "$round" = 2 ]]; then order='transposed reference'; fi
  for binary in $order; do
    for filter in bigint varchar; do
      run="r$round-$filter-$binary"
      args=()
      if [[ "$filter" = varchar ]]; then args+=(-e MVDB_TEXT_FILTER=1); fi
      docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g "${args[@]}" -v "$bin:/probe:ro" -v "$data:/data" --entrypoint "/probe/$binary" mvdb:layout-native "/data/$run/db" 10000 1000 1 1 > "layout-results/$run.json"
      echo "RESULT $run"
      cat "layout-results/$run.json"
    done
  done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
p=Path('layout-results')
for filter in ['bigint','varchar']:
    base=None
    for variant in ['reference','transposed']:
        ds=[json.loads((p/f'r{r}-{filter}-{variant}.json').read_text()) for r in [1,2,3]]
        for r,d in zip([1,2,3],ds):
            assert d['result_digest']==json.loads((p/f'r{r}-{filter}-reference.json').read_text())['result_digest']
        med=statistics.median(d['query_total_ms'] for d in ds)
        if base is None:base=med
        print('LAYOUT_SUMMARY '+json.dumps({'filter':filter,'variant':variant,'query_ms':[d['query_total_ms'] for d in ds],'median_ms':med,'relative_percent':(med/base-1)*100,'exact_results_agree':True}))
PY
