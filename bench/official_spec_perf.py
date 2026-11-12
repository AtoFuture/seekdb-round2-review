#!/usr/bin/env python3
"""复刻官方 MiniVectorDB 性能 benchmark 的本地测量脚本。

官方口径（第 3.3 节）：
  批量写入：从空表开始，每批 100 条写入并提交；耗时含全部批次请求、结果输出与持久化。
  向量 TopK 查询：数据装载和索引准备后执行，含全部查询请求处理和结果输出。
  默认 10,000 条记录、128 维向量、1,000 条 TopK 查询，K=10，一半查询带普通列等值过滤。
  查询默认 exact 模式；无过滤/有过滤两组平均 Recall@10 均须 >= 0.95。

本脚本按同一负载驱动 JSONL 协议，测量：
  T_write / T_query  端到端墙钟时间
  B                   cgroup io.stat 的 rbytes+wbytes 增量（不可得时退化为数据目录字节数）
  M                   cgroup memory.peak（不可得时退化为 docker stats 采样峰值）
并在本地用暴力真值校验 Recall@10，避免"跑得快但结果错"的假优化。
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

VECTOR_SQL_TEMPLATE = "SELECT id, L2_DISTANCE(v, '{query}') AS distance FROM docs{where} ORDER BY distance LIMIT {k};"


def build_image(source: Path, image: str, platform: str | None = None) -> str:
    cmd = ["docker", "build", "-t", image]
    if platform:
        cmd += ["--platform", platform]
    cmd.append(str(source))
    print(f"[build] {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, check=True)
    return image


class JsonlClient:
    """按正式协议与容器交互：先等 ready，再逐条请求逐条响应。"""

    def __init__(self, container_name: str, image: str, data_dir: Path, cpus: str, memory: str,
                 data_mount: str = "bind", binary: str | None = None):
        self.container_name = container_name
        self.data_dir = data_dir
        self.binary = binary
        if binary:
            self.process = subprocess.Popen(
                [binary, "--protocol", "jsonl", "--data-dir", str(data_dir)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", bufsize=1,
            )
        else:
            mount = (f"type=bind,src={data_dir},dst=/data" if data_mount == "bind"
                     else "type=tmpfs,dst=/data,tmpfs-size=1g")
            self.process = subprocess.Popen(
                [
                    "docker", "run", "--rm", "-i", "--name", container_name,
                    "--network=none", "--cpus", cpus, "--memory", memory,
                    "--memory-swap", memory, "--pids-limit=128",
                    "--read-only", "--cap-drop=ALL", "--security-opt=no-new-privileges",
                    "--mount", mount,
                    image,
                ],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", bufsize=1,
            )
        self.counter = 0

    def _readline(self) -> dict:
        line = self.process.stdout.readline()
        if not line:
            err = self.process.stderr.read()
            raise RuntimeError(f"容器提前退出，stderr:\n{err[-4000:]}")
        return json.loads(line)

    def wait_ready(self, timeout: float = 120.0) -> None:
        start = time.monotonic()
        message = self._readline()
        if message != {"type": "ready"}:
            raise RuntimeError(f"握手失败，收到: {message!r}")
        if time.monotonic() - start > timeout:
            raise RuntimeError("握手超时")

    def execute(self, sql: str, search_mode: str = "exact") -> dict:
        self.counter += 1
        request = {"id": self.counter, "op": "execute", "sql": sql, "search_mode": search_mode}
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        response = self._readline()
        if response.get("id") != request["id"]:
            raise RuntimeError(f"响应 id 不匹配: {response!r}")
        return response

    def shutdown(self) -> None:
        self.counter += 1
        request = {"id": self.counter, "op": "shutdown"}
        try:
            self.process.stdin.write(json.dumps(request) + "\n")
            self.process.stdin.flush()
            self._readline()
        except Exception:
            pass
        try:
            self.process.stdin.close()
        except Exception:
            pass
        try:
            self.process.wait(timeout=60)
        except Exception:
            self.process.kill()

    def cgroup_io(self) -> tuple[int, int] | None:
        """读取容器 cgroup 的 io.stat（rbytes, wbytes）。不可得返回 None。"""
        if self.binary:
            return None
        try:
            raw = subprocess.run(
                ["docker", "exec", self.container_name, "cat", "/sys/fs/cgroup/io.stat"],
                capture_output=True, text=True, timeout=20,
            ).stdout
        except Exception:
            return None
        read = write = 0
        found = False
        for line in raw.splitlines():
            parts = line.split()
            if not parts:
                continue
            for item in parts[1:]:
                key, _, value = item.partition("=")
                try:
                    number = int(value)
                except ValueError:
                    continue
                if key == "rbytes":
                    read += number
                    found = True
                elif key == "wbytes":
                    write += number
                    found = True
        return (read, write) if found else None

    def cgroup_memory_peak(self) -> int | None:
        if self.binary:
            return None
        try:
            raw = subprocess.run(
                ["docker", "exec", self.container_name, "cat", "/sys/fs/cgroup/memory.peak"],
                capture_output=True, text=True, timeout=20,
            ).stdout.strip()
            return int(raw) if raw.isdigit() else None
        except Exception:
            return None

    def proc_io(self) -> dict | None:
        """读取容器内数据库进程的 /proc/1/io，作为磁盘写入量的近似。"""
        if self.binary:
            return None
        try:
            raw = subprocess.run(
                ["docker", "exec", self.container_name, "cat", "/proc/1/io"],
                capture_output=True, text=True, timeout=20,
            ).stdout
        except Exception:
            return None
        values = {}
        for line in raw.splitlines():
            key, _, value = line.partition(":")
            value = value.strip()
            if value.isdigit():
                values[key.strip()] = int(value)
        return values or None

    def stderr_tail(self, limit: int = 4000) -> str:
        try:
            return self.process.stderr.read()[-limit:]
        except Exception:
            return ""


def make_rows(rows: int, dim: int, rng: random.Random, categories: int) -> tuple[list[dict], list[str]]:
    """生成确定性数据：cat 用于等值过滤，body 为 VARCHAR 负载。"""
    records = []
    for i in range(1, rows + 1):
        vector = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
        records.append({
            "id": i,
            "cat": i % categories,
            "body": f"row-{i:08d}-" + "x" * 32,
            "vector": vector,
        })
    return records, []


def vector_literal(vector: list[float]) -> str:
    return "[" + ", ".join(f"{value:.6f}" for value in vector) + "]"


def insert_sql(records: list[dict], filter_type: str = "bigint") -> str:
    values = []
    for record in records:
        category = f"'category-{record['cat']}'" if filter_type == "varchar" else str(record["cat"])
        values.append(
            f"({record['id']}, {category}, '{record['body']}', '{vector_literal(record['vector'])}')"
        )
    return "INSERT INTO docs VALUES " + ", ".join(values) + ";"


def truth_topk(records: list[dict], query: list[float], k: int, category: int | None) -> list[int]:
    scored = []
    for record in records:
        if category is not None and record["cat"] != category:
            continue
        total = 0.0
        for a, b in zip(record["vector"], query):
            difference = a - b
            total += difference * difference
        scored.append((math.sqrt(total), record["id"]))
    scored.sort(key=lambda item: (item[0], item[1]))
    return [identifier for _, identifier in scored[:k]]


def run_round(args, records: list[dict], queries: list[dict], round_index: int) -> dict:
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-perf-data-"))
    container_name = f"mvdb-perf-{os.getpid()}-{round_index}"
    client = JsonlClient(container_name, args.image, data_dir, args.cpus, args.memory, args.data_mount, args.binary)
    result: dict = {"round": round_index}
    try:
        client.wait_ready()
        io_before = client.cgroup_io()
        proc_before = client.proc_io()
        # ---- 写入阶段：每批 100 条并自动提交 ----
        write_start = time.monotonic()
        if args.filter_type == "varchar":
            client.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(16), body VARCHAR(64), v VECTOR("
                           f"{args.dim}));")
        else:
            client.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat BIGINT, body VARCHAR(64), v VECTOR("
                           f"{args.dim}));")
        for start in range(0, len(records), args.batch):
            response = client.execute(insert_sql(records[start:start + args.batch], args.filter_type))
            if not response.get("ok"):
                raise RuntimeError(f"写入失败: {response}")
        write_elapsed = time.monotonic() - write_start
        proc_after_write = client.proc_io()
        data_bytes_after_write = sum(p.stat().st_size for p in data_dir.rglob("*") if p.is_file())
        # ---- 索引准备：官方口径下不计入 T，但计入 B/M 与准备时间上限 ----
        index_start = time.monotonic()
        if args.create_index:
            response = client.execute("CREATE VECTOR INDEX docs_vec ON docs(v);")
            if not response.get("ok"):
                raise RuntimeError(f"建索引失败: {response}")
        index_elapsed = time.monotonic() - index_start
        # ---- 查询阶段 ----
        query_start = time.monotonic()
        recalls: list[float] = []
        for item in queries:
            if item["category"] is None:
                where = ""
            elif args.filter_type == "varchar":
                where = f" WHERE cat = 'category-{item['category']}'"
            else:
                where = f" WHERE cat = {item['category']}"
            response = client.execute(
                VECTOR_SQL_TEMPLATE.format(query=vector_literal(item["query"]), where=where, k=args.k),
                args.mode,
            )
            if not response.get("ok"):
                raise RuntimeError(f"查询失败: {response}")
            got = [row[0] for row in response.get("rows", [])]
            expected = item["truth"]
            if expected:
                overlap = len(set(got) & set(expected))
                recalls.append(overlap / min(args.k, len(expected)))
            elif got:
                recalls.append(0.0)
        query_elapsed = time.monotonic() - query_start
        io_after = client.cgroup_io()
        proc_after = client.proc_io()
        memory_peak = client.cgroup_memory_peak()
        data_bytes = sum(p.stat().st_size for p in data_dir.rglob("*") if p.is_file())
        result.update({
            "t_write_s": write_elapsed,
            "t_query_s": query_elapsed,
            "t_index_s": index_elapsed,
            "recall_mean": (sum(recalls) / len(recalls)) if recalls else 0.0,
            "recall_min": min(recalls) if recalls else 0.0,
            "memory_peak_bytes": memory_peak,
            "data_dir_bytes": data_bytes,
            "data_dir_bytes_after_write": data_bytes_after_write,
            "proc_io_write_bytes": (
                proc_after.get("write_bytes", 0) - proc_before.get("write_bytes", 0)
                if proc_after and proc_before else None
            ),
            "proc_io_wchar": (
                proc_after.get("wchar", 0) - proc_before.get("wchar", 0)
                if proc_after and proc_before else None
            ),
            "proc_io_wchar_write_phase": (
                proc_after_write.get("wchar", 0) - proc_before.get("wchar", 0)
                if proc_after_write and proc_before else None
            ),
            "io_rbytes": io_after[0] - io_before[0] if io_before and io_after else None,
            "io_wbytes": io_after[1] - io_before[1] if io_before and io_after else None,
        })
    finally:
        client.shutdown()
        shutil.rmtree(data_dir, ignore_errors=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="MiniVectorDB 官方口径性能复刻")
    parser.add_argument("--image", default=None)
    parser.add_argument("--source", type=Path, help="给定则先 docker build")
    parser.add_argument("--platform", default=None)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--categories", type=int, default=100)
    parser.add_argument("--filter-type", default="bigint", choices=["bigint", "varchar"])
    parser.add_argument("--filtered-ratio", type=float, default=0.5)
    parser.add_argument("--mode", default="exact", choices=["exact", "ann"])
    parser.add_argument("--create-index", action="store_true")
    parser.add_argument("--cpus", default="2")
    parser.add_argument("--memory", default="2g")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--data-mount", default="bind", choices=["bind", "tmpfs"])
    parser.add_argument("--binary", default=None, help="本地可执行文件路径，跳过容器直接测量")
    parser.add_argument("--seed", type=int, default=20261112)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    if not args.image and not args.binary:
        parser.error("需要 --image 或 --binary")
    if args.source:
        build_image(args.source, args.image, args.platform)

    rng = random.Random(args.seed)
    records, _ = make_rows(args.rows, args.dim, rng, args.categories)
    queries = []
    for index in range(args.queries):
        filtered = index % 2 == 0
        category = rng.randrange(args.categories) if filtered else None
        if filtered and rng.random() > args.filtered_ratio:
            filtered = False
            category = None
        query = [rng.uniform(-1.0, 1.0) for _ in range(args.dim)]
        queries.append({
            "query": query,
            "category": category,
            "truth": truth_topk(records, query, args.k, category),
        })

    rounds = []
    for round_index in range(args.repeats):
        outcome = run_round(args, records, queries, round_index)
        rounds.append(outcome)
        print(json.dumps(outcome, ensure_ascii=False), flush=True)

    def median(key: str) -> float | None:
        values = [r[key] for r in rounds if r.get(key) is not None]
        return statistics.median(values) if values else None

    summary = {
        "image": args.image,
        "config": {k: v for k, v in vars(args).items() if k not in {"source", "out", "image"}},
        "t_write_s_median": median("t_write_s"),
        "t_query_s_median": median("t_query_s"),
        "recall_mean": median("recall_mean"),
        "recall_min": median("recall_min"),
        "memory_peak_bytes_median": median("memory_peak_bytes"),
        "data_dir_bytes_median": median("data_dir_bytes"),
        "io_bytes_median": (
            median("io_rbytes") + median("io_wbytes")
            if median("io_rbytes") is not None and median("io_wbytes") is not None else None
        ),
        "rounds": rounds,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if args.out:
        args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
