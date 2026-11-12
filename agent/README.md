# MiniVectorDB 学习材料

正式入口是 `agent/conversation.json`，根目录 `conversation.json` 为逐字节相同副本，必须随源码和 Dockerfile 提交。当前材料 78337 字节，低于官方 128 KiB 限制；包含54条用户记录、39条选取的实验记录、5条Agent观察和2个历史复盘。完整记录保存在 `bench/results/score-iteration-20261112/full-records-before-selection.json`，所选记录与存档逐项相等。

先读 `summary`、`submission_summary`、`formal_feedback` 和内部引用。最新已绑定正式结果是 aea69813：MySQL通过、性能E=1630.670965、Prompt合并无效；具体Prompt原因仍未知。用户新报告一次JSON response timeout、性能未运行，但未提供对应commit，不能归因到新候选，也不能断言Prompt因此被跳过。

第七至十轮证据见 e97–e101：原生x86完整T/B/M对照、实际前一版与正式基线、ARM回退、Unicode/事务/重启验证，以及AVX512乘加融合导致逐位失败后的修复。e102保留LTO构建失败，未纳入生产；e103区分公开协议检查与隐藏超时。实验均有原始轮次和退步记录，不由本地结果换算官方E。

`conversations` 为用户真实发言或标注的节选；`experiments` 为工具执行摘要；`agent_observations`、复盘和模板归于Agent。材料不是完整聊天导出，不补造用户学习反思或未执行的实验。`future_task_prompt` 是尚未执行的新模板。

`material-digest.json` 只核验本地材料完整性，不是平台评审报告或平台内部“材料摘要”字段。`scripts/check-learning-material.py` 检查历史记录不被改写、选取记录不被伪造、82个历史源码Git对象和168个历史证据哈希；`scripts/check-submission-contract.py --check-internal-references` 检查UTF-8、严格JSON、大小、镜像副本和58处内部原文引用。它们不评估Prompt得分，也不证明隐藏准入已通过。
