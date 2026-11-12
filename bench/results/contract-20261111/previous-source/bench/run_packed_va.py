"""Run isolated research prototype on the dedicated D-backed Linux filesystem."""
from pathlib import Path
import argparse, hashlib, json, statistics, subprocess, tempfile
p = argparse.ArgumentParser()
p.add_argument("--source", required=True)
p.add_argument("--output", required=True)
p.add_argument("--rows", type=int, default=2000)
p.add_argument("--queries", type=int, default=40)
p.add_argument("--rounds", type=int, default=3)
args = p.parse_args()
root = Path("/mnt/d/Codex/MiniVectorDB")
output = Path(args.output).resolve()
assert output.is_relative_to(root) and not output.exists()
data_root = Path("/var/lib/minivectordb-portfolio")
data_root.mkdir(parents=True, exist_ok=True)
binary = root / "build/packed_va"
source = root / "repo/experiments/packed_va.cpp"
image = "sha256:cab7b99c76546df33074e0cb6cb08be4f10a53e5bbb7d4b8922aa9fd94fc1324"
docker = ["docker", "--config", "/var/lib/minivectordb-docker-client"]
binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix="owned-prototype-", dir=data_root) as d:
    command = docker + ["create", "--read-only", "--network", "none", "--cpus", "2", "--memory", "512m",
        "--label", "minivectordb.benchmark=portfolio-prototype",
        "--mount", f"type=bind,src={d},dst=/data",
        "--mount", f"type=bind,src={binary},dst=/bench/run,readonly",
        "--entrypoint", "/bench/run", image, "--dir", "/data",
        "--rows", str(args.rows), "--queries", str(args.queries), "--rounds", str(args.rounds)]
    container = subprocess.run(command, check=True, capture_output=True, text=True, timeout=60).stdout.strip()
    assert len(container)==64 and all(c in "0123456789abcdef" for c in container)
    try:
        raw = subprocess.run(docker + ["start", "--attach", container], check=True, capture_output=True, text=True, timeout=1800).stdout
    finally:
        subprocess.run(docker + ["rm", "--force", container], check=True, capture_output=True, text=True, timeout=30)
records = [json.loads(line) for line in raw.splitlines()]
assert len(records) == 6 * args.rounds
for r in records:
    assert r["raw_results_equal"] and r["rows"]==args.rows and r["queries"]==args.queries
    r.update(source=args.source, binary_sha256=binary_hash, benchmark_source_sha256=source_hash,
             image=image, storage="D-backed WSL Linux", resources="2 CPU / 512 MiB")
output.write_text("".join(json.dumps(r)+"\n" for r in records))
assert hashlib.sha256(binary.read_bytes()).hexdigest()==binary_hash
summary = {}
for dataset in ["uniform", "clustered"]:
    summary[dataset] = {}
    for mode in ["one_row", "packed", "packed_va"]:
        group = [r for r in records if r["dataset"]==dataset and r["mode"]==mode]
        assert len(group)==args.rounds
        summary[dataset][mode] = {k:statistics.median(r[k] for r in group) for k in
            ["query_ms", "pages_read", "checksum_bytes", "distance_evaluations", "pruned_rows", "file_bytes", "all_formats_build_ms"]}
report = dict(source=args.source, rows=args.rows, queries=args.queries, rounds=args.rounds,
              medians=summary, validated_queries=len(records)*args.queries)
output.with_suffix(".summary.json").write_text(json.dumps(report, indent=2)+"\n")
print(json.dumps(report, indent=2), flush=True)
