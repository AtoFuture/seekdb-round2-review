#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-first-bench --output /mnt/d/Codex/MiniVectorDB/work/typed-validated-dense.jsonl --rounds 6 --modes reference,typed
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-first-bench --output /mnt/d/Codex/MiniVectorDB/work/typed-validated-indexed.jsonl --rounds 3 --modes indexed_reference,indexed
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-first-bench --output /mnt/d/Codex/MiniVectorDB/work/typed-validated-varied.jsonl --rounds 3 --suite varied --modes reference,typed
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-first-bench --output /mnt/d/Codex/MiniVectorDB/work/typed-validated-profile.jsonl --rounds 2 --profile --modes reference,typed