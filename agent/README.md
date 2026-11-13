# MiniVectorDB 学习材料

入口是 agent/conversation.json，根目录 conversation.json 保持逐字节相同。两份均为 UTF-8 JSON 对象，当前 61,521 字节，低于 128 KiB。

本版按 agent-learning-evidence-v3-unbounded 组织：顶层 rule_version、material_summary，11 个 rubric_instances 覆盖五个维度。每个实例分开给出 student_evidence 与 result_evidence，引用均可在本 JSON 内用 RFC6901 pointer 和原文片段核对；0.5 档不附结果，高于 0.5 档必须有结果。conversations 保留全部 69 条用户原话；experiments/agent_observations 是按 ID 从不可变归档逐字节选取的记录。Agent 的分析、代码和复盘不计入学生证据。

最新已收到的正式结果对应 ff117199：MySQL 通过、E=2576.778405、Prompt 合并无效提示。该提示对应旧版材料；本版按 v3 重构，但不保证平台评分结果。当前 69 条用户记录、23 条实验、5 条 Agent 观察、2 个复盘。

material-digest.json 仅是本地完整性清单，不是平台评分输出。scripts/build-prompt-rubric.py 重建并校验 v3 映射；scripts/check-learning-material.py 校验历史记录、不可变归档、内部引用及历史源码/证据；scripts/check-submission-contract.py 校验精确路径、普通文件、UTF-8、JSON、大小与镜像。它们不评估 Prompt 得分，也不能代替隐藏准入。
