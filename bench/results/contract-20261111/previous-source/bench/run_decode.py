"""Run the decoder-only experiment in a controlled local container."""
import hashlib
import json
import pathlib
import statistics
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()
source = sys.argv[3]
image = sys.argv[4]
project = pathlib.Path("/mnt/d/Codex/MiniVectorDB")
assert binary.is_file() and binary.is_relative_to(project)
assert output.is_relative_to(project) and not output.exists()
docker = ["docker", "--config", "/var/lib/minivectordb-docker-client"]
create = subprocess.run(docker + [
    "create", "--read-only", "--network", "none", "--cpus", "2", "--memory", "512m",
    "--mount", f"type=bind,src={binary},dst=/bench/decode-bench,readonly",
    "--entrypoint", "/bench/decode-bench", image
], text=True, capture_output=True, check=True)
container = create.stdout.strip()
assert len(container) == 64 and all(c in "0123456789abcdef" for c in container)
try:
    completed = subprocess.run(docker + ["start", "--attach", container],
                               text=True, capture_output=True, timeout=600, check=True)
finally:
    subprocess.run(docker + ["rm", "--force", container], text=True,
                   capture_output=True, check=True)
data = json.loads(completed.stdout)
assert len(data["samples"]) == 30
for round_number in [1, 2, 3]:
    for selectivity in [0, 1, 10, 50, 100]:
        pair = [s for s in data["samples"] if s["round"] == round_number and s["match_percent"] == selectivity]
        assert len(pair) == 2 and pair[0]["checksum"] == pair[1]["checksum"]
data.update(library_commit=source, image=image,
            binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            benchmark_source_sha256=hashlib.sha256(pathlib.Path(__file__).with_name("decode.cpp").read_bytes()).hexdigest())
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(data, indent=2)+"\n", encoding="utf-8")
for selectivity in [0, 1, 10, 50, 100]:
    medians = {lazy: statistics.median(s["ms"] for s in data["samples"]
               if s["match_percent"] == selectivity and s["lazy"] == lazy) for lazy in [False, True]}
    print(f"match={selectivity}% eager={medians[False]:.3f}ms lazy={medians[True]:.3f}ms ratio={medians[False]/medians[True]:.4f}")
print("PASS decoder experiment: 30 samples, all paired checksums agree, owned container removed")