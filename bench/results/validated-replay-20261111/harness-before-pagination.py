#!/usr/bin/env python3
"""Local end-to-end T/B/M comparison; never an official score or frozen baseline."""
import argparse
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import random
import re
import stat
import statistics
import struct
import subprocess
import sys
import tempfile
import time

UPSTREAM = "f4d4e26653af823a7601668802f80e374ad6643c"
VENDOR = Path(__file__).resolve().parent / "vendor" / "seekdb-round2-f4d4e26"
DIM = 128


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_io(text, device):
    matches = [line.split() for line in text.splitlines() if line.split() and line.split()[0] == device]
    if len(matches) != 1:
        raise ValueError("missing/duplicate data device in io.stat: " + device)
    pairs = dict(field.split("=", 1) for field in matches[0][1:])
    result = {}
    for field in ("rbytes", "wbytes"):
        if field not in pairs or not pairs[field].isdigit():
            raise ValueError("missing/invalid io.stat " + field)
        result[field] = int(pairs[field])
    return result


def parse_memory(text):
    pairs = dict(line.split() for line in text.splitlines() if line.strip())
    result = {}
    for field in ("file_dirty", "file_writeback"):
        if field not in pairs or not pairs[field].isdigit():
            raise ValueError("missing/invalid memory.stat " + field)
        result[field] = int(pairs[field])
    return result


def data_device(path):
    device = os.stat(path, follow_symlinks=False).st_dev
    return str(os.major(device)) + ":" + str(os.minor(device))


def fsync_owned_tree(root):
    """Fail closed on links/special files; never follow outside the supplied tree."""
    if not hasattr(os, "O_NOFOLLOW"):
        raise RuntimeError("Linux O_NOFOLLOW is required")
    flags = os.O_RDONLY | os.O_NOFOLLOW
    directory_flags = flags | os.O_DIRECTORY
    root_fd = os.open(root, directory_flags)
    counts = {"files": 0, "directories": 0}
    root_device = os.fstat(root_fd).st_dev

    def walk(fd):
        with os.scandir(fd) as entries:
            names = sorted(entry.name for entry in entries)
        for name in names:
            info = os.stat(name, dir_fd=fd, follow_symlinks=False)
            if info.st_dev != root_device:
                raise ValueError("cross-device path in owned data tree")
            if stat.S_ISDIR(info.st_mode):
                child = os.open(name, directory_flags, dir_fd=fd)
                try:
                    opened = os.fstat(child)
                    if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                        raise ValueError("data directory changed during sync")
                    walk(child)
                finally:
                    os.close(child)
            elif stat.S_ISREG(info.st_mode):
                if info.st_nlink != 1:
                    raise ValueError("hardlinked file in owned data tree")
                child = os.open(name, flags, dir_fd=fd)
                try:
                    opened = os.fstat(child)
                    if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                        raise ValueError("data file changed during sync")
                    os.fsync(child)
                    counts["files"] += 1
                finally:
                    os.close(child)
            else:
                raise ValueError("symlink/special file in owned data tree")
        os.fsync(fd)
        counts["directories"] += 1

    try:
        walk(root_fd)
    finally:
        os.close(root_fd)
    return counts


class Resources:
    def __init__(self, session):
        pid = int(session.docker("inspect", "--format", "{{.State.Pid}}", session.name).stdout)
        entries = [line.split(":", 2)[2] for line in Path("/proc", str(pid), "cgroup").read_text().splitlines()
                   if line.startswith("0::")]
        if len(entries) != 1:
            raise ValueError("unified cgroup v2 unavailable")
        self.path = Path("/sys/fs/cgroup") / entries[0].lstrip("/")
        self.path.resolve().relative_to(Path("/sys/fs/cgroup").resolve())
        self.device = data_device(Path(session.bind_data.name))

    def sample(self):
        raw = {name: (self.path / name).read_text().strip()
               for name in ("io.stat", "memory.peak", "memory.current", "memory.swap.current",
                            "memory.stat", "memory.events", "cpu.stat")}
        io = parse_io(raw["io.stat"], self.device)
        dirty = parse_memory(raw["memory.stat"])
        for field in ("memory.peak", "memory.current", "memory.swap.current"):
            if not raw[field].isdigit():
                raise ValueError("invalid " + field)
        return {"at_monotonic_ns": time.monotonic_ns(), "cgroup_path": str(self.path),
                "data_device": self.device, "io": io, "B_bytes": sum(io.values()),
                "M_bytes": int(raw["memory.peak"]), "dirty": dirty,
                "swap_bytes": int(raw["memory.swap.current"]), "raw": raw}


def drain(resources, root, timeout=5.0, interval=0.1):
    before = resources.sample()
    started = time.monotonic()
    counts = fsync_owned_tree(root)
    samples = []
    previous = None
    stable = 0
    while True:
        sample = resources.sample()
        samples.append(sample)
        clean = all(value == 0 for value in sample["dirty"].values())
        stable = stable + 1 if clean and sample["io"] == previous else 0
        if stable >= 2:
            return {"before": before, "after": sample, "samples": samples,
                    "fsync_counts": counts, "elapsed_ms": (time.monotonic() - started) * 1000,
                    "timeout_seconds": timeout, "poll_interval_seconds": interval}
        if time.monotonic() - started >= timeout:
            raise TimeoutError("data writeback did not drain within five seconds")
        previous = sample["io"] if clean else None
        time.sleep(interval)


def make_workload(rows, queries, seed, filter_type):
    rng = random.Random(seed)
    vectors = [[rng.randrange(-2048, 2049) / 128 for _ in range(DIM)] for _ in range(rows)]
    query_vectors = [[rng.randrange(-2048, 2049) / 128 for _ in range(DIM)] for _ in range(queries)]
    order = list(range(rows))
    rng.shuffle(order)
    groups = [i % 10 for i in range(rows)]
    values = groups if filter_type == "bigint" else ["category-" + str(g) for g in groups]
    bodies = ["record-" + str(i) + "-" + chr(97 + i % 26) * 256 for i in range(rows)]
    literal = lambda value: json.dumps(value, separators=(",", ":"))
    group_literal = lambda g: str(g) if filter_type == "bigint" else "'category-" + str(g) + "'"
    inserts = []
    for start in range(0, rows, 100):
        records = ["(%d,%s,'%s','%s')" % (i, group_literal(i % 10), literal(vectors[i]), bodies[i])
                   for i in order[start:start + 100]]
        inserts.append("INSERT INTO t VALUES " + ",".join(records) + ";")
    sqls = ["SELECT id,L2_DISTANCE(v,'%s') AS d,body FROM t%s ORDER BY d LIMIT 10;" %
            (literal(vector), " WHERE g=" + group_literal((j // 2) % 10) if j % 2 else "")
            for j, vector in enumerate(query_vectors)]
    return {"create": "CREATE TABLE t(id BIGINT PRIMARY KEY,g " +
            ("BIGINT" if filter_type == "bigint" else "VARCHAR(32)") +
            ",v VECTOR(128),body VARCHAR(512));",
            "vectors": vectors, "query_vectors": query_vectors, "groups": groups,
            "group_values": values, "bodies": bodies, "inserts": inserts, "queries": sqls}


def verify_rows(response, workload):
    assert response.get("ok") is True, response
    assert response["columns"] == ["id", "g", "v", "body"]
    expected_count = len(workload["vectors"])
    assert len(response["rows"]) == expected_count
    seen = set()
    for row in response["rows"]:
        ident = row[0]
        assert type(ident) is int and 0 <= ident < expected_count and ident not in seen
        seen.add(ident)
        assert row == [ident, workload["group_values"][ident],
                       workload["vectors"][ident], workload["bodies"][ident]], ident


def verify_topk(response, truth, workload):
    assert response.get("ok") is True, response
    assert response["columns"] == ["id", "d", "body"]
    assert len(response["rows"]) == len(truth)
    for row, (ident, distance) in zip(response["rows"], truth):
        assert type(row[0]) is int and row[0] == ident
        assert row[2] == workload["bodies"][ident]
        assert isinstance(row[1], (int, float)) and not isinstance(row[1], bool)
        assert math.isclose(row[1], distance, abs_tol=1e-10, rel_tol=1e-10), (row, ident, distance)


def summarize(samples):
    grouped = {}
    for sample in samples:
        key = sample["version"] + "/" + sample["phase"] + "/" + ("indexed" if sample["indexed"] else "unindexed")
        grouped.setdefault(key, []).append(sample)
    return {key: {"rounds": [row["round"] for row in rows], "samples": rows,
                  "median": {metric: statistics.median(row[metric] for row in rows)
                             for metric in ("T_ms", "B_bytes", "M_bytes")}}
            for key, rows in grouped.items()}


def workload_metadata(queries):
    return {"dimensions": DIM, "batch": 100, "top_k": 10,
            "distribution": "integer uniform [-2048,2048] / 128",
            "half_filtered": queries % 2 == 0,
            "unfiltered_queries": (queries + 1) // 2, "filtered_queries": queries // 2,
            "filter_group_count": 10, "payload_repeated_characters": 256, "search_mode": "exact"}


def sql_workload_hash(workload):
    encoded = json.dumps({key: workload[key] for key in ("create", "inserts", "queries")},
                         separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def close_session(session, result):
    try:
        session.close()
    except BaseException as exc:
        result.update(passed=False, cleanup_error=repr(exc))
        raise


def load_transport():
    lock_path = VENDOR / "upstream-lock.json"
    lock = json.loads(lock_path.read_text())
    if lock["commit"] != UPSTREAM:
        raise ValueError("wrong upstream pin")
    for relative, expected in lock["files"].items():
        path = VENDOR / relative
        path.resolve().relative_to(VENDOR.resolve())
        if not path.is_file() or path.is_symlink():
            raise ValueError("missing/nonregular vendor file: " + relative)
        contents = path.read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(contents)).encode() + b"\0" + contents).hexdigest()
        if blob != expected["git_blob"] or len(contents) != expected["bytes"] or hashlib.sha256(contents).hexdigest() != expected["sha256"]:
            raise ValueError("vendor identity mismatch: " + relative)
    for required in ("protocol_check/_transport.py", "protocol_check/config.json"):
        if required not in lock["files"]:
            raise ValueError("lock misses " + required)
    sys.path.insert(0, str(VENDOR / "protocol_check"))
    session = importlib.import_module("_transport").Session
    config = json.loads((VENDOR / "protocol_check" / "config.json").read_text())
    if config["request_timeout"] != 60 or config["case_timeout"] != 600:
        raise ValueError("unexpected official request/case budget")
    return session, config, sha256(lock_path)


def require_image(image):
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        raise ValueError("images must be immutable sha256 IDs")
    observed = subprocess.run(["docker", "image", "inspect", "--format", "{{.Id}}", image],
                              check=True, capture_output=True, text=True, timeout=30).stdout.strip()
    if observed != image:
        raise ValueError("Docker image identity mismatch")


def oracle(workload, binary, output):
    source = output / "oracle-input.bin"
    with source.open("wb") as stream:
        stream.write(struct.pack("<III", len(workload["vectors"]), len(workload["query_vectors"]), DIM))
        for vector in workload["vectors"] + workload["query_vectors"]:
            stream.write(struct.pack("<128f", *vector))
    truth_path = output / "oracle-truth.txt"
    with truth_path.open("wb") as stream:
        subprocess.run([str(binary), str(source)], stdout=stream, check=True, timeout=600)
    truth = [[(int(pair.split(":")[0]), float(pair.split(":")[1])) for pair in line.split()]
             for line in truth_path.read_text().splitlines()]
    if len(truth) != len(workload["queries"]):
        raise ValueError("wrong oracle query count")
    for j, answer in enumerate(truth):
        population = {i for i in range(len(workload["vectors"])) if not j % 2 or i % 10 == (j // 2) % 10}
        if len(answer) != min(10, len(population)) or len({i for i, _ in answer}) != len(answer):
            raise ValueError("invalid oracle answer length/duplicates")
        if any(i not in population or not math.isfinite(distance) for i, distance in answer):
            raise ValueError("invalid oracle ID/distance")
    return truth, {"input_sha256": sha256(source), "truth_sha256": sha256(truth_path),
                   "oracle_binary_sha256": sha256(binary)}


def run_one(args, Session, config, workload, truth, version, image, round_number, phase, indexed):
    label = "r%d-%s-%s-%s" % (round_number, version, phase, "indexed" if indexed else "unindexed")
    directory = args.output / label
    directory.mkdir()
    result = {"round": round_number, "version": version, "image": image,
              "phase": phase, "indexed": indexed, "filter_type": args.filter_type, "passed": False}
    session = Session(dict(config), directory / "stderr.log", image=image,
                      data_root=str(args.data_root), case_timeout=args.local_case_budget)

    def request(sql):
        started = time.perf_counter_ns()
        response = session.request(sql, mode="exact")
        duration = (time.perf_counter_ns() - started) / 1e6
        if response.get("ok") is not True:
            raise AssertionError(response)
        return response, duration

    try:
        session.start()
        resources = Resources(session)
        result["resources_ready"] = resources.sample()
        request(workload["create"])
        # All INSERT acknowledgements precede the resource drain and persistence verification.
        start = time.perf_counter_ns()
        write_latencies = []
        for sql in workload["inserts"]:
            _, duration = request(sql)
            write_latencies.append(duration)
        write_wall = (time.perf_counter_ns() - start) / 1e6
        result["write_wall_ms"] = write_wall
        result["write_sum_request_ms"] = sum(write_latencies)
        result["write_request_ms"] = write_latencies
        replies = []
        if phase == "query":
            result["index_build_ms"] = request("CREATE VECTOR INDEX tv ON t(v);")[1] if indexed else None
            warm_replies = []
            warm_times = []
            for j in range(args.warmup):
                response, duration = request(workload["queries"][j % args.queries])
                warm_replies.append((j % args.queries, response))
                warm_times.append(duration)
            result["warmup_request_ms"] = warm_times
            result["resources_before_queries"] = resources.sample()
            latencies = []
            start = time.perf_counter_ns()
            for sql in workload["queries"]:
                response, duration = request(sql)
                replies.append(response)
                latencies.append(duration)
            wall = (time.perf_counter_ns() - start) / 1e6
            result.update(T_ms=wall, wall_ms=wall, sum_request_ms=sum(latencies), query_request_ms=latencies)
        else:
            result.update(T_ms=write_wall, wall_ms=write_wall, sum_request_ms=sum(write_latencies))
        result["drain"] = drain(resources, Path(session.bind_data.name))
        finish = result["drain"]["after"]
        result.update(B_bytes=finish["B_bytes"], M_bytes=finish["M_bytes"],
                      data_device=finish["data_device"], swap_bytes=finish["swap_bytes"])
        # End of measured resource window. Host correctness work and restarts follow.
        if phase == "query":
            for j, response in warm_replies:
                verify_topk(response, truth[j], workload)
            for j, response in enumerate(replies):
                verify_topk(response, truth[j], workload)
            result["verified_queries"] = len(replies)
            result["recall_unfiltered"] = 1.0
            result["recall_filtered"] = 1.0
            result["responses_sha256"] = hashlib.sha256(
                json.dumps(replies, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
        verify_rows(request("SELECT id,g,v,body FROM t ORDER BY id;")[0], workload)
        session.shutdown()
        session.start()
        verify_rows(request("SELECT id,g,v,body FROM t ORDER BY id;")[0], workload)
        if phase == "query":
            verify_topk(request(workload["queries"][0])[0], truth[0], workload)
            if args.queries > 1:
                verify_topk(request(workload["queries"][1])[0], truth[1], workload)
        session.shutdown()
        result.update(passed=True, full_rows_before_restart=True, full_rows_after_restart=True,
                      restart_query_checked=(phase == "query"))
    except BaseException as exc:
        result["error"] = repr(exc)
        raise
    finally:
        try:
            close_session(session, result)
        finally:
            write_json(directory / "result.json", result)
    print(json.dumps({key: result[key] for key in ("round", "version", "phase", "indexed", "T_ms", "B_bytes", "M_bytes", "passed")}), flush=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-image", required=True)
    parser.add_argument("--candidate-image", required=True)
    for name in ("output", "data-root", "temp-root", "oracle-bin"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261110)
    parser.add_argument("--filter-type", choices=("bigint", "varchar"), default="bigint")
    parser.add_argument("--local-case-budget", type=int, default=600,
                        help="fixed before execution, includes preparation and post-measurement verification; never changes request timeout")
    args = parser.parse_args(argv)
    if not sys.flags.optimize == 0:
        parser.error("normal Python mode required for correctness assertions")
    if args.rounds < 1 or args.rows < 1 or args.queries < 1 or args.warmup < 0 or args.local_case_budget < 1:
        parser.error("positive rounds/rows/queries/budget and nonnegative warmup required")
    if sys.platform != "linux" or sys.byteorder != "little":
        parser.error("Linux little-endian environment required")
    for name in ("output", "data_root", "temp_root", "oracle_bin"):
        path = getattr(args, name)
        if not path.is_absolute():
            parser.error("explicit absolute paths required: " + name)
    if not args.oracle_bin.is_file():
        parser.error("oracle binary is missing")
    for directory in (args.data_root, args.temp_root):
        directory.mkdir(parents=True, exist_ok=True)
    os.environ.update(TMPDIR=str(args.temp_root), TMP=str(args.temp_root), TEMP=str(args.temp_root),
                      PYTHONDONTWRITEBYTECODE="1")
    tempfile.tempdir = str(args.temp_root)
    Session, config, lock_hash = load_transport()
    require_image(args.reference_image)
    require_image(args.candidate_image)
    args.output.mkdir(parents=True, exist_ok=False)
    workload = make_workload(args.rows, args.queries, args.seed, args.filter_type)
    truth, identity = oracle(workload, args.oracle_bin, args.output)
    metadata = dict(vars(args))
    metadata = {key: str(value) if isinstance(value, Path) else value for key, value in metadata.items()}
    metadata.update(identity, upstream_commit=UPSTREAM, vendor_lock_sha256=lock_hash,
                    harness_sha256=sha256(Path(__file__)), official_config=config,
                    sql_workload_sha256=sql_workload_hash(workload),
                    oracle_source_sha256=sha256(Path(__file__).with_name("jsonl_oracle.cpp")),
                    local_workload=workload_metadata(args.queries),
                    limitations=[
                        "Local generated data, filter selectivity and warmup; no published frozen official generator or baseline.",
                        "Reports per-metric medians and every round, no synthetic official E.",
                        "No global cache reset: host page-cache and WSL environment differ from a frozen scoring host.",
                        "Swap prohibition cannot be assumed from requested Docker memory-swap; observed swap is reported.",
                        "Host-only fsync of owned database files closes the measured writeback window while DB remains alive.",
                        "B/M include startup, data loading, index build and warmup; correctness checks follow the sample.",
                        "Full query loop wall time includes host reply retention; isolated request sum is also reported.",
                        "Resource drain ends before normal shutdown; measures acknowledged durable state, not subsequent shutdown housekeeping."])
    write_json(args.output / "metadata.json", metadata)
    samples = []
    images = [("reference", args.reference_image), ("candidate", args.candidate_image)]
    for r in range(args.rounds):
        versions = images if r % 2 == 0 else list(reversed(images))
        for version, image in versions:
            samples.append(run_one(args, Session, config, workload, truth, version, image, r + 1, "write", False))
        indices = [False, True] if r % 2 == 0 else [True, False]
        for indexed in indices:
            for version, image in versions:
                samples.append(run_one(args, Session, config, workload, truth, version, image, r + 1, "query", indexed))
        write_json(args.output / "summary.json", {"metadata": metadata, "groups": summarize(samples)})
    return 0


if __name__ == "__main__":
    sys.exit(main())

