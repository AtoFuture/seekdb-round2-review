#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
docker --config /var/lib/minivectordb-docker-client run --rm --network none --read-only --entrypoint python3 minivectordb-core:score-first-final -B -c 'from pathlib import Path; import hashlib,json; root=Path("/src"); print(json.dumps({"source_sha256":{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob("*")) if p.is_file()},"binary_sha256":hashlib.sha256(Path("/build/minivectordb").read_bytes()).hexdigest()}))' > work/score-first-image-source.json
docker --config /var/lib/minivectordb-docker-client run --rm --network none --read-only --entrypoint sha256sum minivectordb:score-first-final /usr/local/bin/minivectordb > work/score-first-runtime-binary.txt
python3 -B - <<'PY'
from pathlib import Path
import hashlib,json
root=Path('/mnt/d/Codex/MiniVectorDB')
d=json.loads((root/'work/score-first-image-source.json').read_text())
for name,digest in d['source_sha256'].items():
    assert hashlib.sha256((root/'repo'/name).read_bytes()).hexdigest()==digest, name
assert (root/'work/score-first-runtime-binary.txt').read_text().split()[0]==d['binary_sha256']
print('PASS image source/binary identity',len(d['source_sha256']),d['binary_sha256'])
PY
