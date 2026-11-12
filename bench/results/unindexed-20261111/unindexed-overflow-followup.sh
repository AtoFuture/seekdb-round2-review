#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
python3 repo/bench/run_unindexed.py --binary /mnt/d/Codex/MiniVectorDB/build/unindexed-v4-bench --output /mnt/d/Codex/MiniVectorDB/work/unindexed-v4-overflow-repeat.jsonl --rounds 6 --suite varied --case overflow --modes reference,reuse,scratch
