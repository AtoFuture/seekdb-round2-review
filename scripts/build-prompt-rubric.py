#!/usr/bin/env python3
"""Rebuild agent/conversation.json as an explicit v3 rubric map.

The submitted material had no rubric_instances: reviewers had to infer the five
dimensions from a long record dump. Under agent-learning-evidence-v3-unbounded,
a dimension without a valid student-evidence instance makes the whole material
NEEDS_REVIEW (raw score null), which the platform displays as the combined
invalid/review/range/summary message.

This tool keeps every recorded user statement byte-for-byte, selects only the
experiments/observations needed for the evidence map, and adds one
`rubric_instances` entry per independent problem/experiment/fix chain. Each
entry names exactly one dimension and separates student evidence from result
evidence; 0.5 entries carry no result_evidence.

It never invents a quote, a student statement, or an experiment result.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / "bench/results/score-iteration-20261112/full-records-before-selection.json"
SUPPLEMENT = ROOT / "bench/results/score-next-20261112/iteration-records-appendix.json"
MATERIAL = ROOT / "agent/conversation.json"
MIRROR = ROOT / "conversation.json"
DIGEST = ROOT / "agent/material-digest.json"

DIMENSIONS = [
    "问题理解与自主拆解",
    "技术理解与迁移",
    "独立验证与质疑",
    "实验与反馈迭代",
    "取舍与反思",
]
MAX_BYTES = 128 * 1024
MAX_QUOTE = 240

EXP_IDS = [
    "e10", "e21", "e56",
    "e57", "e58", "e59", "e60", "e61", "e62",
    "e63", "e64", "e65", "e66", "e67", "e68",
    "e71", "e81", "e107", "e110", "e118", "e119", "e123", "e125",
]
OBS_IDS = ["a40", "a44", "a46", "a47", "a50"]


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def pick(rows: list[dict], ids: list[str]) -> list[dict]:
    index = {row.get("id"): row for row in rows}
    missing = [rid for rid in ids if rid not in index]
    if missing:
        raise SystemExit("records missing from archive: " + ", ".join(missing))
    return [index[rid] for rid in ids]


def make_index(rows: list[dict]) -> dict[str, int]:
    return {row["id"]: i for i, row in enumerate(rows)}


def build() -> tuple[dict, dict]:
    archive = load(ARCHIVE)
    supplement = load(SUPPLEMENT)
    records = {field: archive[field] + supplement[field]
               for field in ("conversations", "experiments", "agent_observations")}

    conversations = records["conversations"]
    experiments = pick(records["experiments"], EXP_IDS)
    observations = pick(records["agent_observations"], OBS_IDS)
    data = {
        "conversations": conversations,
        "experiments": experiments,
        "agent_observations": observations,
    }
    index = {field: make_index(rows) for field, rows in data.items()}

    def pointer(field: str, rid: str, text_field: str) -> str:
        if rid not in index[field]:
            raise SystemExit("record missing from material: " + rid)
        row = data[field][index[field][rid]]
        if not isinstance(row.get(text_field), str):
            raise SystemExit(f"{rid} has no string field {text_field}")
        return f"/{field}/{index[field][rid]}/{text_field}"

    def evidence(field: str, rid: str, quote: str, text_field: str = "content") -> dict:
        target = data[field][index[field][rid]].get(text_field)
        if not isinstance(target, str) or quote not in target:
            raise SystemExit(f"quote does not match {rid}: {quote[:40]}")
        if not 0 < len(quote) <= MAX_QUOTE:
            raise SystemExit(f"quote length out of range for {rid}")
        return {"id": rid, "pointer": pointer(field, rid, text_field), "quote": quote}

    def ref(rid: str, pointer_value: str) -> dict:
        return {"id": rid, "pointer": pointer_value}

    def content_ptr(rid: str) -> str:
        return pointer("conversations", rid, "content")

    def output_ptr(rid: str) -> str:
        return pointer("experiments", rid, "output")

    case_unindexed = {
        "id": "case_unindexed",
        "source": "agent_retrospective",
        "title": "无索引弱点：拆分耗时、修复退化、复测",
        "result": "先拆出 60 查询诊断中位 316.443ms，再把主场景从 764.182ms 降到 455.031ms，并以真实 RED 确认测试能抓到退化。",
        "boundary": "Agent 复盘，只用于定位实验链，不作为学生证据；学生证据只在 rubric_instances 中引用一次。",
        "files": [
            "docs/unindexed-results.md",
            "bench/results/unindexed-20261111/manifest.json",
            "bench/results/unindexed-20261111/unindexed-v4-short.summary.json",
            "bench/results/unindexed-20261111/unindexed-resource-v2/result.json",
        ],
    }
    case_score = {
        "id": "case_score_first",
        "source": "agent_retrospective",
        "title": "正式反馈后：把局部提速改为写入/查询的 T、B、M 对照",
        "result": "e66 分别给出写入/查询的 T、累计 B 和峰值 M；e67/e68 验证同一最终程序及资源边界。",
        "boundary": "Agent 复盘，只用于定位实验链，不作为学生证据；学生证据只在 rubric_instances 中引用一次。",
        "files": [
            "bench/results/score-first-20261111/tbm-summary.json",
            "bench/results/score-first-20261111/score-first-image-source.json",
            "bench/results/score-first-20261111/score-first-final-internal.log",
            "bench/results/score-first-20261111/score-first-resource/result.json",
            "bench/results/score-first-20261111/score-memory-varied.summary.json",
            "bench/results/score-first-20261111/core-kernel-pre-default-LastTest.log",
        ],
    }
    review_navigation = {
        "source": "agent_retrospective",
        "purpose": "仅索引真实原文；计分证据只在 rubric_instances 中引用一次，Agent 分析不归为学生理解。",
        "items": [{
            "chain": "无索引弱点与反例修正",
            "result_evidence": [ref(rid, output_ptr(rid))
                                for rid in ("e57", "e58", "e59", "e60", "e61", "e62")],
        }],
    }

    instances: list[dict] = []

    def add(instance_id: str, dimension: str, title: str, problem: str,
            student: list[dict], result: list[dict], rationale: str,
            level: str, attribution_limit: str) -> None:
        instances.append({
            "instance_id": instance_id,
            "dimension": dimension,
            "title": title,
            "problem": problem,
            "student_evidence": student,
            "result_evidence": result,
            "scoring_rationale": rationale,
            "suggested_level": level,
            "attribution_limit": attribution_limit,
        })

    add(
        "P1", DIMENSIONS[0], "把性能目标拆成写入/查询的 T、B、M",
        "性能分由哪些可测量的资源项决定。",
        [evidence("conversations", "u43",
                  "对批量写入和向量 TopK 查询测量耗时 T、磁盘 I/O B、峰值内存 M优化的是这些")],
        [evidence("experiments", "e66",
                  "无索引write1712.011→1322.306ms、query2849.399→1196.348ms，写/查询累计峰值53764096→40648704、61079552→45031424字节",
                  "output")],
        "学生直接指出评分口径是写入/查询的 T、磁盘 I/O B、峰值内存 M；后续实验按这三项分别给出对照值，说明拆解被执行。",
        1.0,
        "口径理解来自学生；实验设计、数值采集与解读由 Agent/工具完成。",
    )
    add(
        "P2", DIMENSIONS[0], "用已得分的 856 版本作基线定位瓶颈",
        "当前候选相对已正式通过的 524133/856 版本差在哪里。",
        [evidence("conversations", "u46", "对比之前 856 分的版本，分析瓶颈在哪里")],
        [evidence("experiments", "e71",
                  "同一已正式通过524133镜像对比本地修复镜像，BIGINT/VARCHAR各3轮，每类18个新会话；总24000个计时查询精确校验、36次完整记录重启核验通过。",
                  "output")],
        "学生指定了对照基线（已正式通过的 856 分版本）、分析目标和优化顺序；实验结果以该镜像为 reference 给出两类过滤的 T/B/M 中位数。",
        1.0,
        "基线与目标由学生指定；实验设计、执行和归因由 Agent 完成。",
    )
    add(
        "T1", DIMENSIONS[1], "先对照两条存储路径，再按结果选择缓存或批量读取",
        "D 盘与 Linux 文件系统路径的差异，以及缓存/批量读取的取舍。",
        [evidence("conversations", "u09",
                  "先做耗时拆分和两种 D 盘存储路径的对照，再根据结果决定缓存还是批量读取")],
        [evidence("experiments", "e21",
                  '"filesystems": {"windows": "UNKNOWN", "linux": "ext2/ext3"}, "groups": {"windows-profile0": {"write_ms": 5180.648287, "write_cpu_ms": 715.623, "query_total_ms": 12790.219605, "query_cpu_ms": 1779.217',
                  "output")],
        "学生要求先做两条路径的对照、再根据结果决定缓存还是批量读取；实验记录了文件系统探测和各分组的写入/查询耗时，说明该决策顺序被实际执行。",
        1.0,
        "决策顺序由学生给出；路径探测、计时与解释由 Agent 完成。",
    )
    add(
        "V1", DIMENSIONS[2], "主动要求刁钻用例反查自家数据库",
        "现有用例是否覆盖容易出错的边界。",
        [evidence("conversations", "u28", "出一点刁钻的问题考考我们的数据库")],
        [evidence("experiments", "e10",
                  "VARCHAR(1)列与'aa'比较，新增测试运行失败：text exceeds scalar length",
                  "output")],
        "学生主动要求构造刁钻问题反查实现；随后实际复现 VARCHAR 长度越界并修复，属于提出反例并拿到实际结果的验证。",
        1.0,
        "反例要求来自学生；具体用例、复现和修复由 Agent 完成。",
    )
    add(
        "V2", DIMENSIONS[2], "拿平台已通过的版本做逐字节差异比对",
        "平台显示成功的版本与自己版本到底差在哪。",
        [evidence("conversations", "u29",
                  "这是一版结果，可以先看看这版格式上和我们有什么区别")],
        [evidence("experiments", "e56",
                  "各50通用检查和58报文，57条逐字节相同，唯一差异为返回距离均为1时旧版主键2、1，新版正确为1、2",
                  "output")],
        "学生要求与平台成功版本做格式和行为对照；实验逐字节比对了 58 条报文并定位到唯一的并列主键顺序差异。",
        1.0,
        "比对要求来自学生；报文采集、比对和差异定位由 Agent 完成。",
    )
    add(
        "V3", DIMENSIONS[2], "对 Prompt 0 分提出接口假设并要求查路径",
        "Prompt 一直无效，是接口/路径问题还是材料问题。",
        [evidence("conversations", "u44",
                  "没分就是接口问题，你要按照他官方仓库给的那个写，json文件里面路径查查有没有写错")],
        [evidence("experiments", "e81",
                  "旧524133的Prompt合并无效原因仍未得到明确类别；本地包装合规不证明平台已读入或评审有效",
                  "output")],
        "学生提出可检验的假设（没分是接口问题）并要求检查官方路径；后续入口被重新生成并核对，但正式反馈仍为合并无效，说明该假设不足以解释问题。",
        1.0,
        "假设由学生提出；路径核对、材料重建和结论由 Agent 完成。",
    )
    add(
        "V4", DIMENSIONS[2], "追问 JSON timeout 是否会影响 Prompt 评价",
        "正确性门槛的 JSON response timeout 与 Prompt 评价是否相关。",
        [evidence("conversations", "u54", "这个可能会影响我们的那个prompt测评吗")],
        [],
        "学生提出一个具体的因果疑问，但材料中没有该问题的确定答案；按 0.5 档只记问题，不补造结果。",
        0.5,
        "问题由学生提出；后续诊断仍标记为未知，未形成确定结论。",
    )
    add(
        "E1", DIMENSIONS[3], "无索引弱点：拆分耗时、修复退化、复测",
        "无索引路径是不是最弱项，成本来自哪里，修复后是否真的改善。",
        [evidence("conversations", "u33", "无索引是我们的弱点部分是吗"),
         evidence("conversations", "u35",
                  "优化完一轮之后就自觉检查自己的薄弱点，然后针对薄弱点继续优化")],
        [evidence("experiments", "e57",
                  "60查询诊断中位316.443ms，回调外85.309、全行校验75.982、物化38.772、距离33.039、pread0.904",
                  "output"),
         evidence("experiments", "e58",
                  "主场景764.182→455.031ms，但每三读一更新完整周期151.067→161.286ms",
                  "output"),
         evidence("experiments", "e60",
                  "旧路径候选向量Cell计数非零而新要求零，真实RED后实现",
                  "output")],
        "学生先问无索引是否是弱点，要求按测量结果进实验，并规定每轮后自查薄弱点再继续；实验先拆出诊断中位 316.443ms，再把主场景从 764.182ms 降到 455.031ms，最后以真实 RED 确认测试能抓到退化。",
        1.0,
        "问题与迭代要求由学生提出；耗时拆分、修复、RED 验证和数据解释由 Agent 完成。",
    )
    add(
        "E2", DIMENSIONS[3], "每一版都与上一版按评分点对照",
        "优化收益是真实提升还是单轮噪声。",
        [evidence("conversations", "u53", "通过每一版与上一版的评分点检验")],
        [evidence("experiments", "e107",
                  "10k×128维、1000查询、BIGINT/VARCHAR过滤、每项3轮中位数对ff117199：写入T=0.4958–0.5450倍、B=0.550442倍、M=0.5105–0.5126倍",
                  "output"),
         evidence("experiments", "e110",
                  "完整查询T相对缓存版0.90669–0.95724倍，M=1.07417–1.08584倍",
                  "output")],
        "学生要求每一版都与上一版按评分点对照；实验以三轮中位数给出相对 ff117199 和上一版的 T/B/M 比值，并保留 M 增加的代价。",
        1.0,
        "对照方法由学生要求；实验设计、统计和结果解释由 Agent 完成。",
    )
    add(
        "R1", DIMENSIONS[4], "按实测收益决定继续叠加还是换路线",
        "定点/整数筛选等路线没有稳定收益时，是继续投入还是放弃。",
        [evidence("conversations", "u64", "有收益就继续叠加优化，没收益就换路线"),
         evidence("conversations", "u65",
                  "以 f62ce8a7 为基线换更有突破性的路线，别再围着没收益的方案打转")],
        [evidence("experiments", "e118",
                  "由此拒绝当前定点实现，不把整数指令或正确性通过等同于性能收益",
                  "output"),
         evidence("experiments", "e119",
                  "写入T下降5.6%–12.9%，查询T下降6.9%–10.5%，B下降23.6%–26.4%",
                  "output")],
        "学生用上一版实测结果设定接受/放弃规则：有收益才叠加，没收益就换路线；实验据此拒绝了变慢的定点实现，并在新日志存储路线上复测出 T/B/M 收益。",
        1.5,
        "接受/放弃规则与换路线决定由学生给出；具体实现、测量和机制解释由 Agent 完成。",
    )
    add(
        "R2", DIMENSIONS[4], "达到数倍 2500 后先提测验证方向",
        "继续优化还是先花一次提交验证方向。",
        [evidence("conversations", "u69",
                  "现在做到的程度是2500多分的几倍，我想先提测一版，看看我们改的方向对不对")],
        [],
        "学生根据当前相对 2500 分的倍数判断继续优化的边际收益，决定先用一次正式提测验证方向；这是有约束的规划取舍，但材料中还没有该次提测的结果，因此按 0.5 档只记规划。",
        0.5,
        "决策由学生提出；提测执行与后续结果尚未记录。",
    )

    document = {
        "summary": "本文件是 MiniVectorDB 加赛的真实开发学习记录（非合成），按 agent-learning-evidence-v3-unbounded 组织为 11 个独立证据实例。全部 69 条学生原话保留；每个实例分开给出学生原文与结果原文，引用可在本 JSON 内用 RFC6901 pointer 和原文片段核对。Agent 的分析、代码和复盘不计入学生证据。",
        "rule_version": "agent-learning-evidence-v3-unbounded",
        "material_summary": "真实用户与 Agent 的开发记录节选：保留 69 条学生原话和关键实验/观察记录；按 v3 五维度列出 11 个独立实例，学生证据与结果证据分列、可定位；Agent 撰写内容明确标注，不计为学生理解。",
        "schema_version": "minivectordb-learning-evidence-curated-v1",
        "material_type": "real",
        "synthetic": False,
        "scope": "本文件只读取自身字符串字段：conversations 保留全部 69 条已记录用户发言；experiments/agent_observations 是按 ID 从团队归档逐字节选取的记录；rubric_instances 只做维度索引，不改写原文。所有计分证据均在本 JSON 字符串内；provenance 中的归档路径只用于来源说明。未记录到的学生理解不予补造。",
        "provenance": {
            "material_kind": "real_student_agent_development_record",
            "user_messages": "69 条全部保留，未改写",
            "results": "从 full-records-before-selection.json 与 iteration-records-appendix.json 按 ID 逐字节选取",
            "agent_authored": ["summary", "material_summary", "submission_summary",
                               "formal_feedback", "material_overview",
                               "agent_observations", "case_studies", "review_navigation"],
            "student_evidence": "rubric_instances[*].student_evidence 指向 conversations 中的学生原话字符串",
        },
        "baseline_commit": "ff117199f8077de7a6e3e1f91a611397bb6641a0",
        "submission_summary": {
            "source": "agent_authored_summary",
            "entry_path": "agent/conversation.json",
            "purpose": "材料索引，不是平台评审报告。",
            "historical_formal": "ff117199 MySQL通过；E=2576.778405；Prompt提示合并无效。该提示对应旧版材料。",
            "candidate": "当前候选 4ffb3656 已完成双机原生 70/70、公开 20/20；相对 ff117199，e123 写入T=0.4382–0.4714、查询T=0.5587–0.6061，e125 写入T=0.4796–0.4939、查询T=0.5314–0.6173。不是官方E换算。",
            "boundary": "本文件按公开 v3 规则显式组织证据，不保证平台评分结果；正式成绩以平台锁定 commit 的评测为准。",
        },
        "formal_feedback": {
            "source": "user_supplied_platform_result",
            "status": "historical_old_material_only",
            "reported_commit": "ff117199f8077de7a6e3e1f91a611397bb6641a0",
            "mysql_admission": "passed",
            "performance_E_text": "2576.778405",
            "prompt_result_text": "Prompt结果无效、待复核、超出平台0–100范围或缺少材料摘要，本项不参与排名；未截断或换算本地分数。",
            "scope": "该提示对应旧版材料；本文件按 v3 规则重构，未对平台评分作任何承诺。",
        },
        "material_overview": {
            "source": "agent_retrospective",
            "prepared_on": "2026-11-13",
            "reading_order": [
                "rule_version 与 material_summary 说明评分规则和材料范围。",
                "rubric_instances 是唯一计分入口：每个实例只归属一个维度，student_evidence 与 result_evidence 来自不同记录。",
                "conversations 保留全部学生原话；experiments 是结果原文；agent_observations 是 Agent 分析，不作为学生证据。",
            ],
            "student_contribution_boundary": "学生提出目标、约束、验证要求和取舍决策；算法、代码、实验执行与机制解释由 Agent 完成。rubric_instances 只把学生原话计为学生证据。",
        },
        "prompt_requirement_evolution": {
            "source": "agent_retrospective",
            "scope": "只归纳原话，不制造历史修订或学生理解。",
            "changes": [
                "u33–u35 要求检查无索引弱点、实际实验并逐轮自检；对应 e57–e62。",
                "u40、u42–u43 反馈正式性能并要求分别测量写入/查询 T/B/M；对应 e63–e68。",
                "u58–u69 要求大幅优化、逐版原生对照并在达到倍数后提测；对应 e107–e125。",
            ],
        },
        "future_task_prompt": {
            "source": "agent_authored",
            "status": "new_template_not_executed",
            "content": "下一步只补齐学生本人对机制与取舍的解释；不把 Agent 复盘当学生证据，不编造未执行的实验。",
        },
        "conversations": conversations,
        "experiments": experiments,
        "agent_observations": observations,
        "case_studies": [case_unindexed, case_score],
        "review_navigation": review_navigation,
        "limitations": [
            "公开和本地用例通过不代表新候选通过正式隐藏准入；正式通过已知仅属于对应已提交 SHA。",
            "平台只给合并 Prompt 状态，没有提供失败码或评审报告。本地输入合规不证明平台已完整读取或评审有效。",
            "Agent 分析、代码和复盘不计入学生证据；未记录到的学生解释没有补造。",
        ],
        "performance_caveats": [
            "自生成固定种子负载、预热和硬件与正式环境不同；不将本地延时比例换算为官方 E。",
            "B/M 包含准备和预热，查询 T 只计查询环节；原生 x86 共享 runner 不是冻结比赛主机。",
        ],
        "scoring_notes": "suggested_level 是本材料按证据给出的最高适用档位（0.5/1/1.5/2），不是平台分数；评审可独立判定档位。",
        "rubric_instances": instances,
    }

    validate(document)
    return document, supplement


def resolve(document: dict, pointer: str) -> str:
    target = document
    for raw in pointer[1:].split("/"):
        part = raw.replace("~1", "/").replace("~0", "~")
        target = target[int(part)] if isinstance(target, list) else target[part]
    if not isinstance(target, str):
        raise SystemExit("evidence pointer is not a string: " + pointer)
    return target


def validate(document: dict) -> None:
    if not isinstance(document, dict):
        raise SystemExit("top level must be an object")
    if document.get("rule_version") != "agent-learning-evidence-v3-unbounded":
        raise SystemExit("wrong rule version")
    if not isinstance(document.get("material_summary"), str) or not document["material_summary"]:
        raise SystemExit("missing material_summary")
    if len(document["conversations"]) != 69:
        raise SystemExit("must retain all 69 user records")
    if any(row.get("role") != "user" and row.get("source") != "user"
           for row in document["conversations"]):
        raise SystemExit("conversations contain a non-user record")

    used_quotes: dict[str, str] = {}
    used_records: dict[str, str] = {}
    dimensions: dict[str, float] = {name: 0.0 for name in DIMENSIONS}
    for instance in document["rubric_instances"]:
        dimension = instance["dimension"]
        if dimension not in DIMENSIONS:
            raise SystemExit("unknown dimension: " + dimension)
        level = float(instance["suggested_level"])
        if level not in (0.5, 1.0, 1.5, 2.0):
            raise SystemExit("invalid level: " + instance["suggested_level"])
        dimensions[dimension] += level
        if level == 0.5 and instance["result_evidence"]:
            raise SystemExit("0.5 instance must not carry result evidence")
        if level > 0.5 and not instance["result_evidence"]:
            raise SystemExit("instance above 0.5 needs result evidence")
        for side in ("student_evidence", "result_evidence"):
            for item in instance[side]:
                target = resolve(document, item["pointer"])
                if item["quote"] not in target:
                    raise SystemExit("quote mismatch: " + item["pointer"])
                if not 0 < len(item["quote"]) <= MAX_QUOTE:
                    raise SystemExit("quote length: " + item["pointer"])
                if item["quote"] in used_quotes:
                    raise SystemExit("quote reused by " + used_quotes[item["quote"]] +
                                     " and " + instance["instance_id"])
                used_quotes[item["quote"]] = instance["instance_id"]
                if item["pointer"] in used_records:
                    raise SystemExit("record reused by " + used_records[item["pointer"]] +
                                     " and " + instance["instance_id"])
                used_records[item["pointer"]] = instance["instance_id"]
    missing = [name for name in DIMENSIONS if dimensions[name] == 0]
    if missing:
        raise SystemExit("dimensions without instances: " + ", ".join(missing))


def dump(document: dict) -> bytes:
    return (json.dumps(document, ensure_ascii=False, indent=1) + "\n").encode("utf-8")


def update_digest(document: dict, data: bytes, supplement: dict) -> None:
    digest = load(DIGEST)
    digest["date"] = "2026-11-13"
    digest["base_commit"] = "a24d2cd764027adec7371f268f99ccdca2307032"
    digest["submission_state"] = (
        "Materials rebuilt on 2026-11-13 as an explicit "
        "agent-learning-evidence-v3-unbounded map. The platform has not "
        "evaluated this rebuild; the latest user-reported result still belongs "
        "to ff117199 and its old-format material."
    )
    digest["material_files"] = {
        "agent/conversation.json": {
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        },
        "conversation.json": {
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        },
    }
    digest["counts"] = {
        "user_records": len(document["conversations"]),
        "experiments": len(document["experiments"]),
        "agent_observations": len(document["agent_observations"]),
        "retrospective_cases": len(document["case_studies"]),
    }
    digest["prompt_rubric"] = {
        "rule_version": document["rule_version"],
        "instances": len(document["rubric_instances"]),
        "dimensions": {
            name: sum(1 for item in document["rubric_instances"]
                      if item["dimension"] == name)
            for name in DIMENSIONS
        },
        "suggested_level_sum": sum(float(item["suggested_level"])
                                   for item in document["rubric_instances"]),
        "note": "suggested_level is a local evidence rating, not a platform score.",
    }
    digest["selection_supplements"] = [
        item for item in digest.get("selection_supplements", [])
        if item.get("path") != "bench/results/score-next-20261112/iteration-records-appendix.json"
    ]
    digest["selection_supplements"].append({
        "path": "bench/results/score-next-20261112/iteration-records-appendix.json",
        "bytes": len(SUPPLEMENT.read_bytes()),
        "sha256": hashlib.sha256(SUPPLEMENT.read_bytes()).hexdigest(),
    })
    DIGEST.write_text(json.dumps(digest, ensure_ascii=False, indent=1) + "\n",
                      encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate without writing")
    args = parser.parse_args()
    document, supplement = build()
    data = dump(document)
    if len(data) > MAX_BYTES:
        raise SystemExit(f"material is {len(data)} bytes, over 128 KiB")
    if args.check:
        print(json.dumps({"passed": True, "bytes": len(data),
                          "instances": len(document["rubric_instances"]),
                          "dimensions": {
                              name: sum(1 for item in document["rubric_instances"]
                                        if item["dimension"] == name)
                              for name in DIMENSIONS}},
                         ensure_ascii=False))
        return 0
    MATERIAL.write_bytes(data)
    MIRROR.write_bytes(data)
    update_digest(document, data, supplement)
    print(json.dumps({"bytes": len(data),
                      "sha256": hashlib.sha256(data).hexdigest(),
                      "instances": len(document["rubric_instances"]),
                      "experiments": len(document["experiments"]),
                      "agent_observations": len(document["agent_observations"])},
                     ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
