# seekdb-round2-review

OceanBase 2026 大赛加赛「赛题一：完成一个轻量向量数据库」（MiniVectorDB）的**独立复核仓库**。

队友的正式开发分支是 `juanzhi-gcb/seekdb` 的 `2026-competition-round2`（复核时的 tip 为 `ff117199f8077de7a6e3e1f91a611397bb6641a0`）。
本仓库**不修改那个分支**，只放复核产出的候选改动，供队友逐条 review 后自行决定采纳哪些。

## 分支

| 分支 | 内容 |
|---|---|
| `codex/round2-review-20261112` | 复核分支，父提交即队友 tip `ff117199`。所有改动都在这里 |
| `main` | 本说明页 |

## 队友怎么用

```sh
# 1) 取回复核分支（不需要本仓库作为 remote 也行）
git clone https://github.com/AtoFuture/seekdb-round2-review.git
cd seekdb-round2-review
git fetch origin codex/round2-review-20261112

# 2) 在自己的克隆里对比（推荐直接看差异）
git remote add review https://github.com/AtoFuture/seekdb-round2-review.git
git fetch review
git diff 2026-competition-round2 review/codex/round2-review-20261112 --stat
git log --oneline 2026-competition-round2..review/codex/round2-review-20261112

# 3) 只想拿其中几条：cherry-pick 单个提交，或按文件取
git checkout review/codex/round2-review-20261112 -- src/sql/executor.cpp
```

改动清单（13 个文件）：

```
CMakeLists.txt                            |  17 +-   ORDER BY 测试注册 + 距离累加的浮点契约
Dockerfile                                |  41 +-   镜像构建加固
README.md                                 |   8 +    新能力说明
src/sql/executor.cpp                      |  55 +    单列 ORDER BY + LIMIT 改有界堆
src/storage/store.cpp                     |  16 +-   数据库文件锁改为有界等待
agent/conversation.json                   | 1836 +-  按五维度重构的 Agent prompt 材料
agent/material-digest.json                | 126 +-  材料摘要（随材料重算）
conversation.json                         | 1836 +-  根目录同内容副本
bench/official_spec_perf.py               | 395 +    题面口径性能复刻脚本（新增）
docs/independent-verification-20261112.md | 139 +    独立复核报告（新增）
docs/prompt-rubric-questions.md           |  59 +    学生必答的 8 个问题（新增）
scripts/build-rubric-material.py          | 471 +    材料重建与自检脚本（新增）
tests/order_by_limit_tests.py             | 176 +    ORDER BY 差分测试（新增）
```

## 复核结论摘要

赛题计分：`最终总分 = 原有赛题 620 + 加赛性能排名 40 + 加赛 Agent prompt 排名 40`，两项加赛分都是独立排名、每降一名减 0.5；
**三项都以「全部正式 MySQL case 通过」为 0/1 门槛**，任一用例失败/崩溃/超时/超资源，本次两项加赛分归零。
性能指标 `E = (E_写入 + 2×E_查询)/3`，查询权重是写入的两倍。

队友已知战绩：正式通过 `524133b`（E=856.55）与 `aea69813`（E=1630.67），但 **Prompt 项两项都是 0**；
另有一次未绑定 commit 的提交报「正确性门槛失败 1 例：JSON response timeout」。
结论是：准入已证明能做到、性能已翻倍，**卡住的是 Prompt 这 40 分和一次没定位的超时**。

本分支针对这两点做了 5 处改动：

1. **单列 ORDER BY + LIMIT 改有界堆**（最可能的超时源）。原路径对全表排序后才截断，还要多趟归并、每条记录两次 seek。
   200000 行同机对照：`ORDER BY bucket ASC LIMIT 10` 598ms → 29ms，`ORDER BY id DESC LIMIT 10` 562ms → 32ms，
   `ORDER BY tag ASC LIMIT 10` 1270ms → 30ms；顺序与并列规则（并列按主键升序）与原排序器逐条一致，
   新增 `tests/order_by_limit_tests.py` 与独立 oracle 比对。
2. **距离累加禁用 FMA 收缩**。aarch64 clang 会把 `sum += d*d` 收缩成 FMA，末位与评测用的 x86_64 基线（无 FMA 指令）不同，
   等距两行的并列次序可能不一致；队友自带的位模式测试在 x86_64 通过、在 ARM64 失败。关闭收缩后两端逐位一致，
   **评测侧行为与性能不变**；代价只有本地 ARM64 查询慢约 11%（ARM 有 FMA 而评测机没有），是否接受由队友定。
3. **数据库文件锁改为有界等待 5 秒**，消除「刚被 SIGKILL 的进程未释放锁 → 启动即退出、ready 永不打印」的竞态。
4. **镜像构建加固**：apk 精确锁版本 + 索引下载抖动会让平台构建硬失败（本地第一次构建就撞上 `samurai (no such package)`，重跑又成功）；
   改为精确锁优先、失败退回次版本模糊匹配并重试，且只编译 `minivectordb` 一个目标。
   同时修掉一个致命细节：Alpine 的 GCC 下 `<queue>` 未包含，clang 侥幸通过但平台构建会失败。
5. **Agent prompt 材料按五维度重构**：新增 14 个实例覆盖 `agent-learning-evidence-v3-unbounded` 的五个维度，
   每个实例给出问题、student_evidence、result_evidence（必来自不同记录）、评分理由与归属边界；
   结果引文改成语义完整、≤240 字符的原文子串（原来 21/21 条都是 64 字符机械截断）；
   恢复归档里带「假设→反证→修正」结构的记录与一批负面结果实验。
   三个校验脚本全部通过（126,182 字节 < 128 KiB，87 条引用全部可定位）。

## 本地验证（不是平台成绩）

- 官方公开题包 `protocol_check/run.py`：**20/20 通过**（ARM64 原生镜像与 amd64 镜像各一轮）。
- 内部回归：44/50 通过；未通过的 6 个套件用 Linux 专用 `--wrap` 链接选项，macOS 上链接不了，队友 tip 原版同样如此。
- 题面口径性能复刻（1 万行×128 维、每批 100 条提交、1000 次 TopK10、一半等值过滤）：
  写入 1.792s、查询 0.620s（VARCHAR 过滤 0.646s）、Recall@10 = 1.0、峰值内存 21.8 MiB、库文件 7.05 MiB。
- 复现脚本在本仓库 `bench/official_spec_perf.py`。

## 仍未解决的风险

1. ANN 模式没有任何剪枝，与 exact 走同一条全扫描路径。
2. 数值列缓存与原始行快照共用 8 MiB 预算，超过约 1.6 万行即 refuse，之后每问退回全表重扫。
3. 超过 64 KiB 的响应先写 `/data` 临时文件再回读，响应体大小直接放大成判题器等待时长。
4. `CREATE VECTOR INDEX` 逐行物化向量，大表上是单请求内的重操作。
5. 新旧格式根页并存时直接判定损坏（启动即退出形态）。

公开用例覆盖不到以上五项，所以公开 20/20 通过不能排除它们；平台那次超时也没有绑定到具体 commit。

## 需要学生本人完成的一件事

规则明确「Agent 的解释、代码和结论不能直接证明学生理解」，任一维度证据不足会让整份材料 NEEDS_REVIEW、原始分为 null。
本分支已把结构补齐，但「技术理解与迁移」「取舍与反思」两个维度仍缺学生自己的解释。
问题清单见 `docs/prompt-rubric-questions.md`（8 个问题，全部基于这一轮真实做过的事），学生用自己的话回答后即可接入材料。

## 说明

本仓库所有内容都是**本地验证**，不构成任何赛事得分或平台回调结果；正式成绩以平台对锁定 commit 的评测为准。
`2026-competition-round2` 分支未被本分支改动。
