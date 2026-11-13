from pathlib import Path
import subprocess, sys, json
root=Path("/mnt/d/Codex/MiniVectorDB")
repo=root/"repo"
source=sys.argv[1]
image="sha256:ccbffecd48173eaadd19951ecb43fba7c5ae003c9dd5a5c82648fac2a1504cb6"
sequence=[("baseline",1),("table",1),("table",2),("baseline",2),("baseline",3),("table",3)]
for mode,round_no in sequence:
    output=repo/f"bench/results/checksum-{mode}-r{round_no}.jsonl"
    binary=root/"build"/("optimization-bench-pre-crc" if mode=="baseline" else "optimization-bench-crc")
    commit="4ad8b99326cbd24523912143bff58b2e286a3c78" if mode=="baseline" else source
    print(f"CHECKSUM COMPARISON {mode} round {round_no}",flush=True)
    subprocess.run(["python3",str(repo/"bench/run_optimization.py"),"--binary",str(binary),"--output",str(output),"--source",commit,"--image",image,"--rows","2000","--queries","40","--rounds","1","--combined-only"],check=True)
print("PASS all six alternating checksum comparisons",flush=True)