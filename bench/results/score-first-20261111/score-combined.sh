#!/bin/sh
set -eu
cd /mnt/d/Codex/MiniVectorDB
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
g++ -std=c++20 -O3 -DNDEBUG -I repo/include repo/bench/typed.cpp build/score-first-core/libminivectordb_core.a -o build/score-combined-bench
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-combined-bench --output /mnt/d/Codex/MiniVectorDB/work/score-combined-dense.jsonl --rounds 6 --modes reference,typed,typed_single
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-combined-bench --output /mnt/d/Codex/MiniVectorDB/work/score-combined-indexed.jsonl --rounds 3 --modes indexed_reference,indexed,indexed_single
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-combined-bench --output /mnt/d/Codex/MiniVectorDB/work/score-memory-dense.jsonl --rounds 6 --modes typed_single,typed_4m,typed_1m
python3 repo/bench/run_typed.py --binary /mnt/d/Codex/MiniVectorDB/build/score-combined-bench --output /mnt/d/Codex/MiniVectorDB/work/score-combined-profile.jsonl --rounds 2 --profile --modes reference,typed_single