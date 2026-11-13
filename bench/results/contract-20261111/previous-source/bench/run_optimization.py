"""Local comparisons. Timings are SQL API timings, not official scores."""
import argparse
import hashlib
import json
import pathlib
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--rows", type=int, default=2000)
parser.add_argument("--queries", type=int, default=40)
parser.add_argument("--rounds", type=int, default=3)
parser.add_argument("--image", default="minivectordb:dev")
parser.add_argument("--source", required=True)
parser.add_argument("--root", default="/mnt/d/Codex/MiniVectorDB/data/optimization-bench")
parser.add_argument("--combined-only", action="store_true", help="measure only selective decode and batch writes together")
parser.add_argument("--batch-only", action="store_true", help="compare decoding with batch writes fixed on")
parser.add_argument("--timeout", type=int, default=1800)
args = parser.parse_args()
assert args.timeout > 0 and args.rounds > 0
root = pathlib.Path(args.root).resolve()
binary = pathlib.Path(args.binary).resolve()
output = pathlib.Path(args.output).resolve()
project = pathlib.Path("/mnt/d/Codex/MiniVectorDB")
for path in [root, binary, output]:
    if not path.is_relative_to(project):
        raise SystemExit(f"all artifacts must stay in the D-drive project: {path}")
assert binary.is_file(), "benchmark executable is missing"
assert not output.exists(), "refusing to overwrite prior experiment output"
root.mkdir(parents=True, exist_ok=True)
output.parent.mkdir(parents=True, exist_ok=True)
orders = [[(0, 0), (1, 1), (1, 0), (0, 1)],
          [(0, 1), (1, 0), (1, 1), (0, 0)],
          [(1, 0), (0, 0), (0, 1), (1, 1)]]
binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
source_hash = hashlib.sha256(pathlib.Path(__file__).with_name("optimization.cpp").read_bytes()).hexdigest()
docker = ["docker", "--config", "/var/lib/minivectordb-docker-client"]
records = []
for round_number in range(args.rounds):
    for decode, batch in orders[round_number % len(orders)]:
        if args.combined_only and not (decode and batch):
            continue
        if args.batch_only and not batch:
            continue
        mode = f"decode{decode}-batch{batch}"
        print(f"START round {round_number+1}/{args.rounds} {mode} rows={args.rows} queries={args.queries}", flush=True)
        with tempfile.TemporaryDirectory(prefix=f"r{round_number+1}-{mode}-", dir=root) as directory:
            command = docker + ["create", "--read-only", "--network", "none", "--cpus", "2",
                       "--memory", "512m", "--env", "MVDB_BENCH_CGROUP=1",
                       "--label", "minivectordb.benchmark=optimization",
                       "--mount", f"type=bind,src={directory},dst=/data",
                       "--mount", f"type=bind,src={binary},dst=/bench/optimization-bench,readonly",
                       "--entrypoint", "/bench/optimization-bench", args.image,
                       "/data/database.mvdb", str(args.rows), str(args.queries), str(decode), str(batch)]
            started = time.monotonic()
            created = subprocess.run(command, text=True, capture_output=True, timeout=60, check=True)
            container = created.stdout.strip()
            assert len(container) == 64 and all(c in "0123456789abcdef" for c in container)
            try:
                result = subprocess.run(docker + ["start", "--attach", container],
                                        text=True, capture_output=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                output.with_suffix(".failure.log").write_text(
                    f"Timeout after {args.timeout} seconds; owned container {container} removed in finally.\n")
                raise SystemExit("benchmark timed out; owned container cleanup requested")
            finally:
                removed = subprocess.run(docker + ["rm", "--force", container],
                                         text=True, capture_output=True, timeout=30)
                if removed.returncode:
                    raise SystemExit(f"owned container cleanup failed: {container}: {removed.stderr}")
            if result.returncode:
                output.with_suffix(".failure.log").write_text(result.stdout + result.stderr)
                raise SystemExit(f"benchmark failed; details: {output.with_suffix('.failure.log')}")
            record = json.loads(result.stdout)
            assert record["validated_queries"] == args.queries
            assert record["recovered_rows"] == args.rows
            record.update(round=round_number+1, mode=mode, source=args.source,
                          binary_sha256=binary_hash, benchmark_source_sha256=source_hash,
                          runner_wall_seconds=time.monotonic()-started, image=args.image,
                          stderr=created.stderr+result.stderr)
            records.append(record)
            with output.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(record, ensure_ascii=False) + "\n")
            print(f"PASS {mode}: write={record['write_ms']:.2f} ms query={record['query_total_ms']:.2f} ms "
                  f"file={record['database_bytes']} vectors={record['vector_values_materialized']}", flush=True)

assert len({r["result_digest"] for r in records}) == 1, "mode results disagree"
assert hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash, "binary changed during experiment"
summary = {}
for mode in sorted({r["mode"] for r in records}):
    subset = [r for r in records if r["mode"] == mode]
    summary[mode] = {key: statistics.median(r[key] for r in subset)
                     for key in ["write_ms", "query_total_ms", "query_filtered_ms", "query_unfiltered_ms",
                                 "database_bytes", "insert_file_growth_bytes", "vector_values_materialized",
                                 "vector_values_filtered", "vector_values_unfiltered", "cells_materialized",
                                 "process_maxrss_kib", "local_cgroup_peak_bytes"]}
report = {"source": args.source, "rows": args.rows, "queries": args.queries, "rounds": args.rounds,
          "batch_only": args.batch_only, "combined_only": args.combined_only,
          "scope": "Local warm SQL API + sink, batch100 autocommits, shuffled inserts, exact truth and reopen checks. "
                   "No stdin/stdout request transport timing; file growth is logical, not physical IO. "
                   "Memory includes benchmark data/driver and local container page cache; swap/IO attribution not official.",
          "medians": summary}
output.with_suffix(".summary.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2), flush=True)