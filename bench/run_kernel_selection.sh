#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
bin="$RUNNER_TEMP/mvdb-kernel-bin"
data="$RUNNER_TEMP/mvdb-kernel-data"
mkdir -p "$bin" "$data" kernel-results
lscpu | tee kernel-results/cpu.txt
docker build --target test --tag mvdb:kernel-native . > kernel-results/build.log 2>&1
docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:kernel-native -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/batch16'
for width in 4 8; do
  docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:kernel-native -c "g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_integrated${width}_probe.cpp /candidate/tests/validated_vector_tests.cpp -o /out/check${width} && /out/check${width} && g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_integrated${width}_probe.cpp /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/batch${width}" 2>&1 | tee "kernel-results/check${width}.log"
done
docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:kernel-native -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_avx2_transposed_probe.cpp /candidate/tests/validated_vector_tests.cpp -o /out/checkavx && /out/checkavx && g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_avx2_transposed_probe.cpp /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/avx2_transposed' 2>&1 | tee kernel-results/check-avx.log
for round in 1 2 3; do
  order='scalar batch4 batch8 batch16 avx2_transposed'
  if [[ "$round" = 2 ]]; then order='avx2_transposed batch16 batch8 batch4 scalar'; fi
  for variant in $order; do
    for filter in bigint varchar; do
      run="r$round-$filter-$variant"
      args=()
      binary="$variant"
      if [[ "$variant" = scalar ]]; then args+=(-e MVDB_SCALAR_KERNEL=1); binary=batch16; fi
      if [[ "$filter" = varchar ]]; then args+=(-e MVDB_TEXT_FILTER=1); fi
      docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g "${args[@]}" -v "$bin:/probe:ro" -v "$data:/data" --entrypoint "/probe/$binary" mvdb:kernel-native "/data/$run/db" 10000 1000 1 1 > "kernel-results/$run.json"
    done
  done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
p=Path('kernel-results')
for filter in ['bigint','varchar']:
    base=None
    for variant in ['scalar','batch4','batch8','batch16','avx2_transposed']:
        ds=[json.loads((p/f'r{r}-{filter}-{variant}.json').read_text()) for r in [1,2,3]]
        for r,d in zip([1,2,3],ds):
            assert d['result_digest']==json.loads((p/f'r{r}-{filter}-scalar.json').read_text())['result_digest']
        med=statistics.median(d['query_total_ms'] for d in ds)
        if base is None:base=med
        print('KERNEL_SUMMARY '+json.dumps({'filter':filter,'variant':variant,'query_ms':[d['query_total_ms'] for d in ds],'median_ms':med,'relative_percent':(med/base-1)*100,'exact_results_agree':True}))
PY
