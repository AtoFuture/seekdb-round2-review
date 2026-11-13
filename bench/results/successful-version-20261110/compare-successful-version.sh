#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
docker build --network host --target runtime -t minivectordb:platform-success-8b7054 work/successful-version-20261110/source-8b7054 > work/successful-version-20261110/build-old.log 2>&1
docker build --network host --target runtime -t minivectordb:success-compare-latest repo > work/successful-version-20261110/build-latest.log 2>&1
docker image inspect --format '{{.Id}}' minivectordb:platform-success-8b7054 minivectordb:success-compare-latest > work/successful-version-20261110/images.txt
cat work/successful-version-20261110/images.txt
python3 work/compare-successful-version.py minivectordb:platform-success-8b7054 minivectordb:success-compare-latest
python3 work/run-official-on-d.py --image minivectordb:platform-success-8b7054 --package work/official-public-v4/mysqltest --output work/successful-version-20261110/public-old
python3 work/run-official-on-d.py --image minivectordb:success-compare-latest --package work/official-public-v4/mysqltest --output work/successful-version-20261110/public-latest
