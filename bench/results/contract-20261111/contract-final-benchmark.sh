#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
export DOCKER_CONFIG=/var/lib/minivectordb-docker-client
runtime=$(docker image inspect --format '{{.Id}}' minivectordb:contract-final)
for filter in bigint varchar; do
  python3 -B repo/bench/run_contract_benchmark.py --reference-image sha256:91fe268d07c2c0da849ef87dc00eebb96e5a9df5c2f4d9f41ac27b05ce6da856 --candidate-image "$runtime" --output "/mnt/d/Codex/MiniVectorDB/work/contract-final-$filter" --data-root /var/lib/minivectordb-contract-bench --temp-root /mnt/d/Codex/MiniVectorDB/tmp --oracle-bin /mnt/d/Codex/MiniVectorDB/work/score-first-jsonl-oracle --rounds 3 --rows 10000 --queries 1000 --warmup 10 --filter-type "$filter" > "work/contract-final-$filter.log" 2>&1
  cat "work/contract-final-$filter.log"
done
