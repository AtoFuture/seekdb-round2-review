#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
export DOCKER_CONFIG=/var/lib/minivectordb-docker-client
docker build --network host --target test -t minivectordb-core:contract-final repo > work/contract-final-build-test.log 2>&1
docker build --network host --target runtime -t minivectordb:contract-final repo > work/contract-final-build-runtime.log 2>&1
docker image inspect --format '{{.Id}}' minivectordb-core:contract-final minivectordb:contract-final > work/contract-final-images.txt
cat work/contract-final-images.txt
docker run --rm --network none --cpus 2 --memory 512m --mount type=bind,src=/mnt/d/Codex/MiniVectorDB/data,dst=/data minivectordb-core:contract-final > work/contract-final-internal.log 2>&1
tail -4 work/contract-final-internal.log
docker run --rm --network none --read-only --entrypoint python3 minivectordb-core:contract-final -B -c 'from pathlib import Path; import hashlib,json; root=Path("/src"); print(json.dumps({"source_sha256":{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob("*")) if p.is_file()},"binary_sha256":hashlib.sha256(Path("/build/minivectordb").read_bytes()).hexdigest()}))' > work/contract-final-image-source.json
docker run --rm --network none --read-only --entrypoint sha256sum minivectordb:contract-final /usr/local/bin/minivectordb > work/contract-final-runtime-binary.txt
python3 -B repo/bench/vendor/seekdb-round2-f4d4e26/protocol_check/run.py --image minivectordb:contract-final --output /mnt/d/Codex/MiniVectorDB/work/contract-final-public > work/contract-final-public.log 2>&1
tail -4 work/contract-final-public.log
python3 -B work/protocol-extra-checks.py minivectordb:contract-final contract-final-model > work/contract-final-model.log 2>&1
tail -4 work/contract-final-model.log
python3 -B repo/tests/runtime_smoke.py minivectordb:contract-final data/contract-final-runtime > work/contract-final-runtime.log 2>&1
cat work/contract-final-runtime.log
