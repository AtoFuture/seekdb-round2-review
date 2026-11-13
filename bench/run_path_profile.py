"""D-drive Windows bind vs D-backed MiniVectorDB Linux filesystem; no global changes."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,sys,statistics
project=Path("/mnt/d/Codex/MiniVectorDB")
repo=project/"repo"
binary=project/"build/profile-bench"
output=repo/"bench/results/path-profile.jsonl"
source=sys.argv[1]
assert not output.exists()
image="sha256:2680aa18f52ae4f62c989d49788225e3ac0eba6e9276fe71bb46477a9d1d9438"
roots={"windows":project/"data/path-profile","linux":Path("/var/lib/minivectordb-profile")}
# This exact distro's registry BasePath was verified as D:\Codex\MiniVectorDB\wsl.
# Caller must repeat that verification before using this runner on another host.
docker=["docker","--config","/var/lib/minivectordb-docker-client"]
for root in roots.values():root.mkdir(parents=True,exist_ok=True)
filesystem={k:subprocess.check_output(["stat","-f","-c","%T",str(v)],text=True).strip() for k,v in roots.items()}
hashes={"binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest(),"benchmark_source_sha256":hashlib.sha256((repo/"bench/profile.cpp").read_bytes()).hexdigest()}
orders=[[("windows",False),("linux",False),("linux",True),("windows",True)],
        [("windows",True),("linux",True),("linux",False),("windows",False)],
        [("linux",False),("windows",False),("windows",True),("linux",True)]]
records=[]
for round_no,order in enumerate(orders,1):
    for storage,profiling in order:
        print(f"START round={round_no} storage={storage} profile={profiling}",flush=True)
        with tempfile.TemporaryDirectory(prefix="owned-",dir=roots[storage]) as directory:
            command=docker+["create","--read-only","--network","none","--cpus","2","--memory","512m",
                "--label","minivectordb.benchmark=path-profile","--env","MVDB_BENCH_CGROUP=1",
                "--mount",f"type=bind,src={directory},dst=/data",
                "--mount",f"type=bind,src={binary},dst=/bench/profile,readonly",
                "--entrypoint","/bench/profile"]
            if profiling:command+=["--env","MVDB_PROFILE=1"]
            command += [image,"/data/database.mvdb","2000","40","1","1"]
            created=subprocess.run(command,check=True,capture_output=True,text=True,timeout=60)
            container=created.stdout.strip()
            assert len(container)==64 and all(c in "0123456789abcdef" for c in container)
            try:
                run=subprocess.run(docker+["start","--attach",container],check=True,capture_output=True,text=True,timeout=900)
            finally:
                subprocess.run(docker+["rm","--force",container],check=True,capture_output=True,text=True,timeout=30)
            r=json.loads(run.stdout)
            assert r["validated_queries"]==40 and r["recovered_rows"]==2000 and r["profile_enabled"]==profiling
            r.update(round=round_no,storage=storage,source=source,image=image,filesystem=filesystem[storage],**hashes)
            records.append(r)
            with output.open("a") as f:f.write(json.dumps(r)+"\n")
            print(f"PASS write={r['write_ms']:.3f} query={r['query_total_ms']:.3f} cpu={r['query_cpu_ms']:.3f}",flush=True)
for key in ["result_digest","database_bytes","insert_file_growth_bytes","vector_values_materialized","distances_computed"]:
    assert len({r[key] for r in records})==1,key
assert hashlib.sha256(binary.read_bytes()).hexdigest()==hashes["binary_sha256"]
groups={}
for storage in roots:
    for profiling in [False,True]:
        rs=[r for r in records if r["storage"]==storage and r["profile_enabled"]==profiling]
        med={k:statistics.median(r[k] for r in rs) for k in ["write_ms","write_cpu_ms","query_total_ms","query_cpu_ms"]}
        for phase in ["write_profile","query_profile"]:
            med[phase]={name:{k:statistics.median(r[phase][name][k] for r in rs) for k in ["calls","bytes","nanoseconds"]} for name in rs[0][phase]}
        groups[f"{storage}-profile{int(profiling)}"]=med
summary={"source":source,"filesystems":filesystem,"groups":groups,"validated_queries":480,"reopen_checks":12,**hashes}
output.with_suffix(".summary.json").write_text(json.dumps(summary,indent=2)+"\n")
print(json.dumps(summary,indent=2),flush=True)