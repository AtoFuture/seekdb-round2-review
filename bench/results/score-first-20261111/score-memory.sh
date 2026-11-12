#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-combined-bench --output /mnt/d/Codex/MiniVectorDB/work/score-memory-varied.jsonl --rounds 3 --suite varied --modes typed_single,typed_4m