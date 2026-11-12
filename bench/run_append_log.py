"""Controlled isolated one/two-sync append-log experiment, not SQL throughput."""
from pathlib import Path
import argparse,hashlib,json,statistics,subprocess,tempfile
p=argparse.ArgumentParser()
p.add_argument("--source",required=True); p.add_argument("--output",required=True)
p.add_argument("--transactions",type=int,default=100); p.add_argument("--rounds",type=int,default=3)
args=p.parse_args()
root=Path("/mnt/d/Codex/MiniVectorDB"); out=Path(args.output).resolve()
assert out.is_relative_to(root) and not out.exists()
binary=root/"build/append_log"; source=root/"repo/experiments/append_log.cpp"
image="sha256:a5cfe6055ddcf5a5d88c1e89bb17c685f3efa1e46d6b1b8696982716f56b985f"
docker=["docker","--config","/var/lib/minivectordb-docker-client"]
data_root=Path("/var/lib/minivectordb-compact"); data_root.mkdir(parents=True,exist_ok=True)
binary_hash=hashlib.sha256(binary.read_bytes()).hexdigest()
source_hash=hashlib.sha256(source.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix="owned-log-",dir=data_root) as data:
    cmd=docker+["create","--read-only","--network","none","--cpus","2","--memory","512m",
        "--label","minivectordb.benchmark=append-log",
        "--mount",f"type=bind,src={data},dst=/data",
        "--mount",f"type=bind,src={binary},dst=/bench/run,readonly",
        "--entrypoint","/bench/run",image,"--bench","/data",str(args.transactions),str(args.rounds),"100","800"]
    container=subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=60).stdout.strip()
    assert len(container)==64 and all(c in "0123456789abcdef" for c in container)
    try: raw=subprocess.run(docker+["start","--attach",container],check=True,capture_output=True,text=True,timeout=1800).stdout
    finally:subprocess.run(docker+["rm","--force",container],check=True,capture_output=True,text=True,timeout=30)
records=[json.loads(line) for line in raw.splitlines()]
assert len(records)==2*args.rounds
for row in records:
    row.update(source=args.source,binary_sha256=binary_hash,experiment_source_sha256=source_hash,
               image=image,storage="D-backed WSL Linux",resources="2 CPU / 512 MiB")
out.write_text("".join(json.dumps(row)+"\n" for row in records))
assert hashlib.sha256(binary.read_bytes()).hexdigest()==binary_hash
assert hashlib.sha256(source.read_bytes()).hexdigest()==source_hash
print(json.dumps(records,indent=2),flush=True)

assert {r["mode"] for r in records}=={"one_sync","two_sync"}
assert len({r["file_bytes"] for r in records})==1
for r in records:
    assert r["exact_live_and_replay"] and r["transactions"]==args.transactions and r["rows_per_transaction"]==100 and r["payload_bytes"]==800
    assert r["fsync_calls"]==args.transactions*(2 if r["mode"]=="two_sync" else 1)
summary={m:{k:statistics.median(r[k] for r in records if r["mode"]==m) for k in
    ["write_seconds","replay_seconds","reopen_seconds","file_bytes","write_bytes","write_calls","fsync_calls"]}
    for m in ["two_sync","one_sync"]}
out.with_suffix(".summary.json").write_text(json.dumps(dict(source=args.source,transactions=args.transactions,rounds=args.rounds,medians=summary),indent=2)+"\n")
print(json.dumps(summary,indent=2),flush=True)
