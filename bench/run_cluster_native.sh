#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = x86_64
bin="$RUNNER_TEMP/mvdb-cluster-bin"
data="$RUNNER_TEMP/mvdb-cluster-data"
mkdir -p "$bin" "$data" cluster-results
lscpu | tee cluster-results/cpu.txt
docker build --target test --tag mvdb:cluster-native . > cluster-results/build.log 2>&1
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g mvdb:cluster-native 2>&1 | tee cluster-results/ctest.log
docker run --rm --network none -v "$PWD:/candidate:ro" -v "$bin:/out" --entrypoint sh mvdb:cluster-native -c 'g++ -std=c++20 -O3 -I/src/include /candidate/bench/score_kernel_diagnostic.cpp /build/libminivectordb_core.a -o /out/probe'
for round in 1 2 3; do
  order='previous cluster'
  if [[ "$round" = 2 ]]; then order='cluster previous'; fi
  for variant in $order; do
    for filter in bigint varchar; do
      run="r$round-$filter-$variant"
      args=()
      if [[ "$variant" = previous ]]; then args+=(-e MVDB_CLUSTER_OFF=1); fi
      if [[ "$filter" = varchar ]]; then args+=(-e MVDB_TEXT_FILTER=1); fi
      docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g "${args[@]}" -v "$bin:/probe:ro" -v "$data:/data" --entrypoint /probe/probe mvdb:cluster-native "/data/$run/db" 10000 1000 1 1 > "cluster-results/$run.json"
    done
  done
done
python3 - <<'PY'
import json,statistics
from pathlib import Path
p=Path('cluster-results')
for f in ['bigint','varchar']:
    for variant in ['previous','cluster']:
        ds=[json.loads((p/f'r{r}-{f}-{variant}.json').read_text()) for r in [1,2,3]]
        for r,d in zip([1,2,3],ds):
            assert d['result_digest']==json.loads((p/f'r{r}-{f}-previous.json').read_text())['result_digest']
        print('CLUSTER_SUMMARY '+json.dumps({'filter':f,'variant':variant,'medians':{k:statistics.median(d[k] for d in ds) for k in ['query_total_ms','query_unfiltered_ms','query_filtered_ms','write_ms','process_maxrss_kib']},'samples':ds,'exact_results_agree':True}))
PY
