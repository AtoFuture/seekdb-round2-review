# MiniVectorDB 学习材料

入口是 agent/conversation.json，根目录 conversation.json 保持逐字节相同。两份均为 UTF-8 JSON 对象，低于 128 KiB，随代码与 Dockerfile 提交。

先读 summary、submission_summary 和 formal_feedback。最新已收到的正式结果对应 ff117199：MySQL 通过、E=2576.778405、Prompt 合并无效提示。此后实验不是正式成绩，当前仍不知道 Prompt 无效的具体原因。

本轮新增记录见 u55 起、e104 起。用户要求以 T/B/M 逐版对照、保留失败和退步；最新决定先提交一版验证优化方向。已完成的日志存储、响应复用、容量和小数据诊断附原生证据。

conversations 保留实际用户发言或标明的节选；experiments 是执行摘要；agent_observations、复盘与未来模板明确归于 Agent。不是完整聊天导出，不补造用户理解，不声称未完成的实验成功。

material-digest.json 仅是本地完整性清单，不是平台评分输出或内部材料摘要。scripts/check-learning-material.py 校验历史记录、不可变归档、内部引用及历史源码/证据；scripts/check-submission-contract.py 校验精确路径、普通文件、UTF-8、JSON、大小与镜像。它们不评估 Prompt 得分，也不能代替隐藏准入。
