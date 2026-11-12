"""Same-binary compact SQL experiments in the dedicated D-backed Linux disk."""
from pathlib import Path
import argparse, hashlib, json, statistics, subprocess, tempfile
p=argparse.ArgumentParser()
p.add_argument("--source",required=True)
p.add_argument("--output",required=True)
p.add_argument("--rows",type=int,default=2000)
p.add_argument("--queries",type=int,default=40)
p.add_argument("--rounds",type=int,default=3)
p.add_argument("--paired-cache",type=int)
p.add_argument("--diagnostic",action="store_true")
p.add_argument("--suite",choices=["standard","varied"],default="standard")
args=p.parse_args()
root=Path("/mnt/d/Codex/MiniVectorDB"); repo=root/"repo"
out=Path(args.output).resolve()
assert out.is_relative_to(root) and not out.exists()
binary=root/"build/compact-bench"
binary_hash=hashlib.sha256(binary.read_bytes()).hexdigest()
source_hash=hashlib.sha256((repo/"bench/compact.cpp").read_bytes()).hexdigest()
image="sha256:a5cfe6055ddcf5a5d88c1e89bb17c685f3efa1e46d6b1b8696982716f56b985f"
docker=["docker","--config","/var/lib/minivectordb-docker-client"]
data_root=Path("/var/lib/minivectordb-compact")
data_root.mkdir(parents=True,exist_ok=True)
cases={"standard":{}}
if args.suite=="varied":
    cases={"ordered":{"MVDB_ORDERED":"1"},"wide":{"MVDB_PAYLOAD":"5000"},
           "dim16":{"MVDB_DIMENSIONS":"16","MVDB_PAYLOAD":"0"},
           "dim768":{"MVDB_DIMENSIONS":"768"},"filter1":{"MVDB_GROUPS":"100"},
           "churn":{"MVDB_CHURN":"1"}}
modes=[(False,0),(True,0),(True,262144),(True,1048576)]
if args.paired_cache is not None:modes=[(False,0),(True,args.paired_cache)]
records=[]
for case,extra in cases.items():
    for round_no in range(args.rounds):
        order=modes if round_no%2==0 else list(reversed(modes))
        if len(modes)>2 and round_no%3==2:order=order[2:]+order[:2]
        for packed,budget in order:
            mode=f"packed{int(packed)}-scan{budget}"
            print(f"START {case} r{round_no+1} {mode}",flush=True)
            with tempfile.TemporaryDirectory(prefix="owned-",dir=data_root) as data:
                env={"MVDB_SCAN_CACHE_BYTES":str(budget),"MVDB_BENCH_CGROUP":"1",**extra}
                if packed:env["MVDB_PACKED"]="1"
                if args.diagnostic:env["MVDB_PROFILE"]="1"
                cmd=docker+["create","--read-only","--network","none","--cpus","2","--memory","512m",
                            "--label","minivectordb.benchmark=compact",
                            "--mount",f"type=bind,src={data},dst=/data",
                            "--mount",f"type=bind,src={binary},dst=/bench/run,readonly","--entrypoint","/bench/run"]
                for k,v in env.items():cmd+=["--env",f"{k}={v}"]
                cmd += [image,"/data/database.mvdb",str(args.rows),str(args.queries),"1","1"]
                container=subprocess.run(cmd,check=True,capture_output=True,text=True,timeout=60).stdout.strip()
                assert len(container)==64 and all(x in "0123456789abcdef" for x in container)
                try:
                    result=subprocess.run(docker+["start","--attach",container],capture_output=True,text=True,timeout=1800)
                    if result.returncode:
                        out.with_suffix(".failure.log").write_text(result.stdout+"\n"+result.stderr)
                        raise RuntimeError(f"benchmark {case}/{mode} failed: {result.stderr}")
                    row=json.loads(result.stdout)
                finally:
                    subprocess.run(docker+["rm","--force",container],check=True,capture_output=True,text=True,timeout=30)
                expected_rows=args.rows*3//4 if case=="churn" else args.rows
                assert row["validated_queries"]==args.queries and row["recovered_rows"]==expected_rows
                assert row["packed_values"]==packed and row["scan_cache_budget"]==budget and row["profile_enabled"]==args.diagnostic
                row.update(case=case,round=round_no+1,mode=mode,source=args.source,binary_sha256=binary_hash,
                           benchmark_source_sha256=source_hash,image=image,storage="D-backed WSL Linux")
                records.append(row)
                with out.open("a") as f:f.write(json.dumps(row)+"\n")
                print(f"PASS write={row['write_ms']:.3f} query={row['query_total_ms']:.3f} churn={row['churn_ms']:.3f} bytes={row['database_bytes']}",flush=True)
for case in cases:
    group=[r for r in records if r["case"]==case]
    for key in ["result_digest","distances_computed","rows_examined","vector_values_materialized","recovered_rows"]:
        assert len({r[key] for r in group})==1,(case,key)
    for packed in [False,True]:
        for key in ["database_bytes","insert_file_growth_bytes","churn_file_growth_bytes"]:
            assert len({r[key] for r in group if r["packed_values"]==packed})==1,(case,packed,key)
assert hashlib.sha256(binary.read_bytes()).hexdigest()==binary_hash
assert hashlib.sha256((repo/"bench/compact.cpp").read_bytes()).hexdigest()==source_hash
summary={}
for case in cases:
    summary[case]={}
    for mode in sorted({r["mode"] for r in records if r["case"]==case}):
        group=[r for r in records if r["case"]==case and r["mode"]==mode]
        v={k:statistics.median(r[k] for r in group) for k in ["write_ms","query_total_ms","write_cpu_ms","query_cpu_ms",
            "churn_ms","churn_cpu_ms","database_bytes","insert_file_growth_bytes","churn_file_growth_bytes",
            "process_maxrss_kib","local_cgroup_peak_bytes"]}
        if args.diagnostic:
            for phase in ["write_profile","query_profile"]:
                v[phase]={n:{k:statistics.median(r[phase][n][k] for r in group) for k in ["calls","bytes","nanoseconds"]}
                          for n in group[0][phase]}
        summary[case][mode]=v
report=dict(source=args.source,rows=args.rows,queries=args.queries,rounds=args.rounds,diagnostic=args.diagnostic,
            medians=summary,validated_queries=len(records)*args.queries,reopen_checks=len(records))
out.with_suffix(".summary.json").write_text(json.dumps(report,indent=2)+"\n")
print(json.dumps(report,indent=2),flush=True)
