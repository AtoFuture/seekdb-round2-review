#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
docker --config /var/lib/minivectordb-docker-client build --network host --target test -t minivectordb-core:score-first-final repo > work/score-first-final-build-test.log 2>&1
docker --config /var/lib/minivectordb-docker-client build --network host --target runtime -t minivectordb:score-first-final repo > work/score-first-final-build-runtime.log 2>&1
docker --config /var/lib/minivectordb-docker-client image inspect --format '{{.Id}}' minivectordb-core:score-first-final minivectordb:score-first-final > work/score-first-final-images.txt
cat work/score-first-final-images.txt
docker --config /var/lib/minivectordb-docker-client run --rm --network none --cpus 2 --memory 512m --mount type=bind,src=/mnt/d/Codex/MiniVectorDB/data,dst=/data minivectordb-core:score-first-final > work/score-first-final-internal.log 2>&1
tail -4 work/score-first-final-internal.log
python3 work/run-official-on-d.py --image minivectordb:score-first-final --package work/official-public-v4/mysqltest --output work/score-first-final-public
python3 work/protocol-extra-checks.py minivectordb:score-first-final score-first-final-model
python3 repo/tests/runtime_smoke.py minivectordb:score-first-final data/score-first-final-runtime > work/score-first-final-runtime.log 2>&1
cat work/score-first-final-runtime.log
g++ -std=c++20 -O2 repo/bench/jsonl_oracle.cpp -o work/score-first-jsonl-oracle
runtime=$(docker --config /var/lib/minivectordb-docker-client image inspect --format '{{.Id}}' minivectordb:score-first-final)
python3 repo/bench/run_jsonl_versions.py --current-image "$runtime" --reference-image sha256:91fe268d07c2c0da849ef87dc00eebb96e5a9df5c2f4d9f41ac27b05ce6da856 --reference-label 524133baf0a4ed947805236c02cce9cecffc0be3 --current-label score-first-final-working-tree --output-prefix jsonl-score-first-final --rounds 3 --oracle-bin /mnt/d/Codex/MiniVectorDB/work/score-first-jsonl-oracle

python3 work/score-first-resource-check.py minivectordb:score-first-final > work/score-first-resource.log 2>&1
