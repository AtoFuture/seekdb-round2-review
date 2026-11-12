# 独立复核与本轮改动：2026-11-12

对象：`2026-competition-round2` 分支 tip（`ff117199f8077de7a6e3e1f91a611397bb6641a0`）。
本记录由独立复核方在队友工作区之外重新构建、重新测量得到，所有数字都来自本轮实际执行，不是平台成绩。

## 一、这道题到底在计什么

加赛总分 = 原有赛题 620 分 + 加赛性能排名分 40 + 加赛 Agent prompt 排名分 40，两项加赛分都是「独立排名、每降一名减 0.5」。

**三项都建立在一个 0/1 门槛上：全部正式 MySQL case 必须通过。** 任一用例错误、崩溃、超时或资源超限，
本次提交两项加赛分都是 0。所以优先级是：先保证准入不炸，再谈性能排名，最后才是 Prompt 排名。

性能排名用 `E = (E_写入 + 2 × E_查询) / 3`，其中每项 `E = 0.6×f(R_T) + 0.2×f(R_B) + 0.2×f(R_M)`，
`f(R) = R/(1+R)`，`R` 是「基线 / 本机」效率比。**查询权重是写入的两倍**，写入与查询的时间、磁盘 I/O、峰值内存都进指标。
注意平台的 E 是原始口径数值（例如 1630.670965），不是 0–100 制。

## 二、当前真实战绩（来自平台反馈，材料里也有记录）

| 提交 | MySQL 准入 | 性能 | Prompt |
|---|---|---|---|
| `524133b` | 通过 | E = 856.552980 | 合并无效，本项 0 |
| `aea69813` | 通过 | E = 1630.670965 | 无效、待复核，本项 0 |
| 未绑定 commit | **正确性门槛失败 1 例：JSON response timeout** | 未执行 | 未执行 |

结论：**准入已经证明能做到，性能也翻了一倍多；真正拿不到分的是 Prompt 这一项，而且曾经出现过一次超时。**
这两个才是当前该解决的问题，性能已经不是主要矛盾。

## 三、本轮独立复核结果

| 检查 | 结果 |
|---|---|
| 官方公开题包 `protocol_check/run.py`（20 例 / 95 条 SQL） | tip 上 20/20 通过；本轮改动后同样 20/20 通过（Linux amd64 容器） |
| 题面口径性能复刻（10000 行 × 128 维，每批 100 条提交，1000 次 TopK10，一半等值过滤） | 原生 ARM64 容器 2CPU/2GiB：写入 1.792s，查询 0.620s（BIGINT 过滤）/ 0.646s（VARCHAR 过滤），Recall@10 = 1.0，峰值内存 21.8 MiB，库文件 7.05 MiB，进程累计写出 7.8 MB |
| 内部回归（macOS ARM64 原生构建） | 43/49 通过；6 个「Not Run」是 Linux 专用 `--wrap` 链接选项在 macOS 无法链接，与本次改动无关（tip 原版同样如此） |
| 位模式距离测试 `core_validated_vector` | **tip 原版在 ARM64 上失败、在 x86_64 通过**（见第五节 FMA） |

## 四、本轮改动

### 1. 单列 ORDER BY + LIMIT 改用有界堆（消除最可能的超时源）

外部排序路径对每一条匹配行都入缓冲，`finish()` 无论 LIMIT 多小都把全表排完再截断，还要多趟归并、每条记录额外两次 seek。
这与「单请求 60 秒上限」叠加，就是大表上 `ORDER BY ... LIMIT 10` 超时的最可能原因。

新路径只保留 K 个候选，顺序与 `sql::Sorter` 完全一致（`compare_cells` 比较、并列按主键升序，升降序都一致）。
主键升序时扫描本身按主键有序，可以取满 K 行即停。

200000 行同一台机器对照（macOS ARM64 原生）：

| 语句 | tip 原版 | 本轮 | 变化 |
|---|---:|---:|---:|
| `ORDER BY bucket ASC LIMIT 10` | 598 ms | 29 ms | 20.6× |
| `ORDER BY id DESC LIMIT 10` | 562 ms | 32 ms | 17.6× |
| `ORDER BY tag ASC LIMIT 10` | 1270 ms | 30 ms | 42.3× |

新增 `tests/order_by_limit_tests.py`（已注册进 ctest）：600 行、含大量并列值，覆盖升降序、7 种 LIMIT、带过滤、主键排序、
VARCHAR 排序键、多列排序回退、投影同行，逐条与独立 oracle 比对顺序与内容。

### 2. 距离累加禁用 FMA 收缩（跨平台数值一致性）

`core_validated_vector` 在 x86_64 通过、在 ARM64 失败。根因：aarch64 clang 会把 `sum += d*d` 收缩成 FMA，
末位与 x86_64 不同，等距两行的并列次序可能因此不同。评测镜像跑在 x86_64 基线上（无 FMA 指令），所以这只影响本地 ARM64 验证，
但后果是**本地 ARM64 通过、平台 x86_64 未必通过**。

改法：只对真正做距离累加的编译单元（`src/vector/exact.cpp`、`src/sql/binder.cpp`）加 `-ffp-contract=off`，
并让位模式对照的测试用同一口径。改完 ARM64 与 x86_64 逐位一致（1249 个位级用例通过），x86_64 评测行为不变。

代价：本地 ARM64 查询慢约 11%（0.388s → 0.430s，1000 次查询），因为 ARM 有 FMA 而评测机没有。
**这个 11% 只存在于本地 ARM64**；平台侧数值与性能都不受影响。是否接受由队友决定，若要保 ARM64 本地速度，可退回全局不设该选项。

### 3. 崩溃恢复的文件锁改为有界等待

`flock(LOCK_EX|LOCK_NB)` 遇到「上一个进程刚被 SIGKILL、锁还没释放」会立刻失败退出，`ready` 永不打印。
改为最多等待 5 秒再报错，消掉这个竞态。

### 4. Dockerfile 构建加固

原来 `apk add build-base=0.5-r3 cmake=3.31.7-r1 samurai=1.2-r7 python3=3.12.16-r0` 全部精确锁版本。
本地第一次构建就因为索引下载不完整报 `samurai (no such package)`——**同一个命令随后又能成功**，说明是索引/网络抖动，
不是版本被下架。但 alpine 补丁版本被 supersede 时精确锁同样会硬失败，而镜像构建失败等于整份提交 0 分。

改法：精确锁优先，失败后退回 apk 的模糊匹配（`build-base~0.5` 等，仍锁次版本），并对索引下载重试三次；
构建阶段只编译 `minivectordb` 一个目标（测试目标挪到 test 阶段按需构建），缩短平台构建时间。

### 5. 学习材料按五维度重构（Prompt 项当前 0 分）

平台对 `aea69813` 的原话是「Prompt 结果无效、待复核、超出平台 0–100 范围或缺少材料摘要」。
按 `agent-learning-evidence-v3-unbounded`，**任一维度证据不足就会让整份材料 NEEDS_REVIEW、原始分为 null**，
所以这不是「材料不好看」，而是有维度缺学生自己的证据。

原材料的结构性问题（脚本实测）：
- 全文没有「问题理解 / 自主拆解 / 独立验证 / 质疑 / 取舍 / 反思 / 维度」等任何维度词，也没有维度→实例映射；
- 21/21 条结果引文都是 output 的前 64 字符机械截断，好几处切在数字中间（`…距离33.039、pread0.`）；
- 5 条结果引文只有 CI run id 或 commit sha，没有任何指标或判定词，充当不了「实际结果」；
- 58 个引用对象里有 21 个 pointer 重复出现两次，跨实例重复计分风险高。

本轮做法：新增 `rubric_instances`（14 个独立实例，五个维度全部覆盖，每个只归属一个维度），
每个实例给出问题、`student_evidence`（学生原话）、`result_evidence`（实际结果，两者必来自不同记录）、
`scoring_rationale`（为什么算这一档）和 `attribution_limit`（哪部分是学生、哪部分是 Agent）；
引文改成语义完整、≤240 字符、且是该记录原文的子串；恢复归档里带「假设→反证→修正」结构的观察记录与一批负面结果实验；
在 summary 里说明平台的 E 是性能原始口径、不是 0–100 制分数；补上 `summary` 顶层字段并修正「53 条」与实际 54 条的矛盾。
构建脚本 `scripts/build-rubric-material.py` 可重复执行，改材料后重新生成摘要与校验。

**剩下的缺口只有学生本人能补**：规则明确「Agent 的解释、代码和结论不能直接证明学生理解」。
现在「技术理解与迁移」「取舍与反思」两个维度的学生证据仍然只有操作性发言，
题目与要求见 [prompt-rubric-questions.md](prompt-rubric-questions.md)（8 个问题，全部基于这一轮真实做过的事）。

## 五、仍未解决的风险（按可能性排序）

1. **ANN 模式没有任何剪枝**：`ann` 与 `exact` 走同一条全扫描路径（`src/sql/executor.cpp` 的 search_mode 分支）。
   题面只要求 Recall@10 ≥ 0.95，但全扫描意味着近似检索没有任何速度优势，同时把数据量放大直接变成单请求耗时。
2. **缓存超预算后每问都全表重扫**：数值列缓存与原始行快照共用 8 MiB 预算，超过约 1.6 万行（128 维）即 refuse，
   之后每次查询都退回逐行解码 + 逐行算距离。题面要求「数据量大于内存预算」的场景正好落在这里。
3. **大响应走临时文件再回读**：超过 64 KiB 的响应先写 `/data` 临时文件再回读输出，换行符只在最后产生，
   响应体大小直接放大成判题器等待时长；`/data` 写满时该请求会失败。
4. **CREATE VECTOR INDEX 逐行物化**：建索引时每行都构造一个 `vector<float>`，大表上是单请求内的重操作。
5. **恢复期的格式双根冲突**：新旧格式根页并存时 `a[7]!=b[7]` 直接判定损坏，属于启动即退出的形态。

这五项没有一项在公开用例里出现，所以公开 20/20 通过不能排除它们；平台那一次超时报告也没有绑定到任何 commit，
无法直接定位到上面哪一条。

## 六、复现命令

```sh
# 官方公开用例
python3 protocol_check/run.py --image <image> --output <new-dir>

# 题面口径性能复刻（本轮脚本）
python3 bench/official_spec_perf.py --image <image> --rows 10000 --queries 1000 --repeats 3 \
        --filter-type bigint --out <result.json>

# 内部回归与材料自检
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2
MVDB_TEST_DIR=/data/core-tests ctest --test-dir build --output-on-failure
python3 scripts/build-rubric-material.py
python3 scripts/check-learning-material.py
python3 scripts/check-submission-contract.py --check-internal-references
```

以上全部是本地验证，不能换算成任何赛事得分；正式结果以平台对锁定 commit 的评测为准。
