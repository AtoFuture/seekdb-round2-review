"""Factorial and paired production SQL experiments, in verified D-backed WSL."""
from pathlib import Path
import argparse,hashlib,json,subprocess,tempfile,statistics
a=argparse.ArgumentParser()
a.add_argument("--source",required=True)
a.add_argument("--output",required=True)
a.add_argument("--rows",type=int,default=2000)
a.add_argument("--queries",type=int,default=40)
a.add_argument("--rounds",type=int,default=3)
a.add_argument("--paired-cache",type=int)
a.add_argument("--diagnostic",action="store_true")
args=a.parse_args()
root=Path("/mnt/d/Codex/MiniVectorDB")
repo=root/"repo"
binary=root/"build/portfolio-bench"
output=Path(args.output).resolve()
assert output.is_relative_to(root) and not output.exists()
data_root=Path("/var/lib/minivectordb-portfolio")
data_root.mkdir(parents=True,exist_ok=True)
image="sha256:cab7b99c76546df33074e0cb6cb08be4f10a53e5bbb7d4b8922aa9fd94fc1324"
docker=["docker","--config","/var/lib/minivectordb-docker-client"]
modes=[(crc,budget) for crc in ["bytewise","slicing8"] for budget in [0,262144,1048576]]
if args.paired_cache is not None:modes=[("bytewise",0),("slicing8",args.paired_cache)]
binary_hash=hashlib.sha256(binary.read_bytes()).hexdigest()
source_hash=hashlib.sha256((repo/"bench/portfolio.cpp").read_bytes()).hexdigest()
records=[]
for round_no in range(args.rounds):
    order=modes if round_no%2==0 else list(reversed(modes))
    if len(modes)>2 and round_no%3==2:order=order[3:]+order[:3]
    for crc,budget in order:
        print(f"START r{round_no+1} {crc} cache={budget} rows={args.rows} queries={args.queries} diagnostic={args.diagnostic}",flush=True)
        with tempfile.TemporaryDirectory(prefix="owned-",dir=data_root) as d:
            command=docker+["create","--read-only","--network","none","--cpus","2","--memory","512m",
                "--label","minivectordb.benchmark=portfolio","--env","MVDB_BENCH_CGROUP=1",
                "--env",f"MVDB_CACHE_BYTES={budget}",
                "--mount",f"type=bind,src={d},dst=/data","--mount",f"type=bind,src={binary},dst=/bench/run,readonly",
                "--entrypoint","/bench/run"]
            if crc=="bytewise":command+=["--env","MVDB_CRC_BYTEWISE=1"]
            if args.diagnostic:command+=["--env","MVDB_PROFILE=1"]
            command += [image,"/data/database.mvdb",str(args.rows),str(args.queries),"1","1"]
            c=subprocess.run(command,check=True,capture_output=True,text=True,timeout=60).stdout.strip()
            assert len(c)==64 and all(x in "0123456789abcdef" for x in c)
            try:r=json.loads(subprocess.run(docker+["start","--attach",c],check=True,capture_output=True,text=True,timeout=1800).stdout)
            finally:subprocess.run(docker+["rm","--force",c],check=True,capture_output=True,text=True,timeout=30)
            assert r["validated_queries"]==args.queries and r["recovered_rows"]==args.rows
            assert r["cache_budget"]==budget and r["crc_bytewise"]==(crc=="bytewise") and r["profile_enabled"]==args.diagnostic
            r.update(round=round_no+1,mode=f"{crc}-cache{budget}",source=args.source,binary_sha256=binary_hash,benchmark_source_sha256=source_hash,image=image,storage="D-backed WSL Linux")
            records.append(r)
            with output.open("a") as f:f.write(json.dumps(r)+"\n")
            print(f"PASS write={r['write_ms']:.3f} query={r['query_total_ms']:.3f} write_hits={r['write_cache']['hits']} query_hits={r['query_cache']['hits']}",flush=True)
for key in ["result_digest","database_bytes","insert_file_growth_bytes","distances_computed","vector_values_materialized"]:
    assert len({r[key] for r in records})==1,key
assert hashlib.sha256(binary.read_bytes()).hexdigest()==binary_hash
summary={}
for mode in sorted({r["mode"] for r in records}):
    group=[r for r in records if r["mode"]==mode]
    summary[mode]={k:statistics.median(r[k] for r in group) for k in ["write_ms","query_total_ms","write_cpu_ms","query_cpu_ms","database_bytes","insert_file_growth_bytes","process_maxrss_kib","local_cgroup_peak_bytes"]}
    for phase in ["write_cache","query_cache"]:
        summary[mode][phase]={k:statistics.median(r[phase][k] for r in group) for k in group[0][phase]}
    if args.diagnostic:
        for phase in ["write_profile","query_profile"]:
            summary[mode][phase]={name:{k:statistics.median(r[phase][name][k] for r in group) for k in ["calls","bytes","nanoseconds"]} for name in group[0][phase]}
report={"source":args.source,"rows":args.rows,"queries":args.queries,"rounds":args.rounds,"diagnostic":args.diagnostic,"medians":summary,"validated_queries":len(records)*args.queries,"reopen_checks":len(records)}
output.with_suffix(".summary.json").write_text(json.dumps(report,indent=2)+"\n")
print(json.dumps(report,indent=2),flush=True)
