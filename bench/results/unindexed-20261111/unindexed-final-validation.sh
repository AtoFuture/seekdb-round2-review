#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
docker --config /var/lib/minivectordb-docker-client build --network host --target test -t minivectordb-core:unindexed-final repo > work/unindexed-final-build-test.log 2>&1
docker --config /var/lib/minivectordb-docker-client build --network host --target runtime -t minivectordb:unindexed-final repo > work/unindexed-final-build-runtime.log 2>&1
docker --config /var/lib/minivectordb-docker-client image inspect --format '{{.Id}}' minivectordb-core:unindexed-final minivectordb:unindexed-final > work/unindexed-final-images.txt
cat work/unindexed-final-images.txt
docker --config /var/lib/minivectordb-docker-client run --rm --network none --cpus 2 --memory 512m --mount type=bind,src=/mnt/d/Codex/MiniVectorDB/data,dst=/data minivectordb-core:unindexed-final > work/unindexed-final-internal.log 2>&1
tail -4 work/unindexed-final-internal.log
python3 work/run-official-on-d.py --image minivectordb:unindexed-final --package work/official-public-v4/mysqltest --output work/unindexed-final-public
python3 work/protocol-extra-checks.py minivectordb:unindexed-final unindexed-final-model
python3 repo/tests/runtime_smoke.py minivectordb:unindexed-final data/unindexed-final-runtime > work/unindexed-final-runtime.log 2>&1
cat work/unindexed-final-runtime.log
g++ -std=c++20 -O2 repo/bench/jsonl_oracle.cpp -o work/unindexed-jsonl-oracle
runtime=$(docker --config /var/lib/minivectordb-docker-client image inspect --format '{{.Id}}' minivectordb:unindexed-final)
python3 repo/bench/run_jsonl_versions.py --current-image "$runtime" --reference-image sha256:e1cdb1e7239b915403b846f0c78ab19a4d032e58ffc77c6aba6d05552a38bc07 --reference-label 64f43d1bf56af8a19eace2ddf44bae40807e6c40 --current-label unindexed-final-working-tree --output-prefix jsonl-unindexed-final --rounds 3 --oracle-bin /mnt/d/Codex/MiniVectorDB/work/unindexed-jsonl-oracle
