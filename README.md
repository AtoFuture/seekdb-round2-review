# MiniVectorDB

第二轮独立开发分支 2026-competition-round2。支持磁盘表目录、行编解码、SQL 执行、精确向量查询、持久化向量索引和事务，默认使用官方 f4d4e266 发布的 JSONL 协议。本轮延续 a7c88df6 的实现，以正式得分 1630.670965 的 aea69813bb6cfb1b0abea3d892db0d4c32517829 为主要对照；856.552980 的 524133 保留为历史版本。实验结果、退步及失败见[本轮证据目录](bench/results/score-iteration-20261112)。本地耗时不换算为正式 7000 分或 Prompt 40 分。

## 当前能力

- CREATE TABLE；BIGINT、VARCHAR(n)、VECTOR(n)；一个 BIGINT 主键，每表最多一个向量列。
- INSERT（列清单及多行 VALUES）、SELECT（普通投影、*、L2_DISTANCE）、UPDATE 常量赋值、DELETE。
- 普通列 =、<、<=、>、>= 及 AND；多列 ORDER BY、ASC/DESC、LIMIT；并列按主键升序。
- VARCHAR 按 Unicode 字符计数和码点比较；向量校验有限 float32 和维度，距离使用存储后的值。
- 精确和带过滤的 TopK；按返回的欧氏距离排序，并列按主键升序；事务中读取自己的向量更新。
- CREATE VECTOR INDEX；磁盘平坦向量索引与表数据一起提交、回滚，新增、更新和删除同步维护。exact 和 ann 均使用完整精确检索，不宣称近似剪枝或索引提速。
- 单个磁盘提交根覆盖表目录和所有表；自动提交、显式 BEGIN/COMMIT/ROLLBACK、语句失败保存点。
- 双版本提交记录、同步后确认、正常关闭回滚及异常进程退出后恢复。
- 流式扫描和输出；主键等值直接查找；一般排序按预算生成临时段并做两路归并。

完整语法仍以正式题包为准。不支持的 NULL、JOIN、OR、聚合、子查询、ALTER/DROP 和主键更新会报错，不会伪装成功。

## 存储与内存边界

4096 字节页、显式小端编码、校验及跨页记录。所有表共享一个文件，以“表命名空间+有符号主键”定位。新 SQL 数据库默认格式04：省去只有一个子节点的 radix 层级，并将稀疏节点以位图和非空指针编码、合并存入不可变页面。主键查找与普通扫描直接跳过压缩路径。格式02/03文件继续使用原格式，不自动迁移；旧版二进制不能读取04，旧格式01仍明确拒绝。

格式02/03的单键修改仍可能追加16个索引页以及记录页；04减少这些路径复制和稀疏页面浪费。INSERT/UPDATE/DELETE 仍按不超过128条、累计512 KiB的小批次共享新建路径（单条超大记录单独处理）。节点打包额外使用一页暂存及有界编码缓冲区，最后一页写完才发布工作根；同页子节点必须先于父节点，拒绝自引用和前向引用。旧页面保持不可变，语句保存点仍恢复共同工作根。SQL 默认使用1 MiB索引页缓存和4 MiB数据页缓存，预算没有扩大。尚无空间回收，不要求全部记录常驻内存。排序预算是工作状态目标，不等于进程硬限制；强杀可能留下临时排序目录。

引号内的普通数值向量使用无临时词法 token 的快速识别路径，先验证完整语法，再调用原有 double/float32 转换。带注释或其他拼写回退到原词法器；6150个差分用例比较了数值位模式及完整错误，避免用改舍入或放宽输入换取写入速度。

向量 TopK 仅在 K 能落入预算时走有界堆，否则走排序路径。从存储读取行时检查整行格式、UTF-8 和有限向量，默认同步索引路径还核验索引与原表的主键集合及向量一致性。默认开启 `typed_scan_cache`：对于无过滤、BIGINT 过滤或至多一个 VARCHAR 列过滤的合适 TopK，将完整验证后的向量、BIGINT 列及可选的一个 VARCHAR 列保存在自有对齐数组中，后续查询复用验证结果，避免重复解析无关文本和复制向量；过滤、距离和赢家仍每次重新计算。多个 VARCHAR 过滤列或其他不适用的路径保留原始行快照或流式扫描。

数值列缓存与原始行快照互斥，共用 `scan_snapshot_bytes` 的默认及硬上限 8 MiB，不是各占 8 MiB。首次符合条件的扫描仅观察，第二次完整扫描才构建；预算不足、超容量或构建失败不会发布部分缓存。每条已解析的非 SELECT 在绑定/执行前失效缓存，包括失败的写入、DDL 和事务命令。缓存预算包含向量、BIGINT 数组、可选 VARCHAR 槽位与长度及列映射；固定对象、分配器、当前行与查询临时状态另计。缓存不持久化，不能代替磁盘表和索引。

同表、同索引一旦选择原始行快照，后续无过滤或整数过滤查询继续使用它，直到失效或扫描身份切换。观察、构建及超预算拒绝状态都遵守这一选择，避免字符串过滤与无过滤交替时反复清空两套缓存。可容纳的单 VARCHAR 列现在优先进入类型化缓存；列切换需重新构建，宽列超预算则维持原始快照回退。

`validated_snapshot_replay=true` 允许 TopK 复用完整原始行快照已有的 UTF-8、有限向量与索引一致性校验证明。重放仍检查结构边界及主键，按实际谓词比较字符串视图，只复制命中的向量；每次重新计算距离、过滤和赢家，赢家回查仍完整校验。此内部视图只接受 READY 状态的自有不可变字节，不用于首次磁盘读取、部分构建或调用方缓冲区。预算、失效规则和公开解码器不变；关闭该选项可对照逐行重复校验路径。超过 8 MiB 的完整扫描不能使用此优化。

`validated_vector_kernel=true` 可在 SQL 已证明候选向量有限时选用内部距离入口：查询向量每次仍验证一次，每个候选仍检查维度，省去重复的逐分量有限性检查。double 累加顺序、sqrt 后排序及有符号主键并列规则保持不变；公开 `exact_top_k` / `l2_distance` 仍完整检查输入。设为 false 使用检查版对照。UPDATE/DELETE 仍完整解码行，赢家投影仍回查原表；不能据此推断物理 I/O 一定下降。

第七至十轮在上述总预算内增加：完整 16 行块一次性原地转置、单 VARCHAR 列的有界类型化表示、按普通列排序的行缓存，以及运行时 AVX2/AVX-512 分派。排序仅在排列数组和单行暂存各不超过 64 KiB 时进行，移动所有列并保留主键关联；转置尾部仍按行存储。每次查询重新执行谓词、距离、TopK 与赢家回查，不缓存答案。AVX-512 严格禁止乘加融合，经真实支持该指令的主机逐位检查；不支持的 CPU 回退。缓存写入/事务失效规则不变。

新建向量索引采用目录 v2 的 28 字节引用记录，向量仍从同一提交根的原表读取；读取时核对完整主键集合及记录身份。旧目录 v1 仍可读取，旧二进制不能读取 v2。此变化减少重复落盘的向量，不削弱持久化与一致性检查。

SQL 默认提交策略为 `CommitSyncPolicy::ProcessCrash`：完成所有新页的写入循环后写提交根，再对整个文件执行一次 `fsync`，成功返回后才确认提交。恢复承诺针对进程被 SIGKILL、内核及文件系统继续存活的正式崩溃模型；不将未确认提交遇到断电、内核或设备故障纳入同一保证。`Ordered` 保留先同步数据、再写根并同步的两道屏障，底层 Store 默认仍使用 Ordered。提交 I/O 失败后数据库进入结果不确定状态，须关闭并重新打开，不能继续使用可能失真的事务视图。

## 开发和运行

历史实验使用 D:/Codex/MiniVectorDB 下的 WSL 环境。本轮使用 macOS 上的 Lima/Ubuntu 24.04 ARM64 虚拟机，原生 ARM64 容器进行同机性能对照，x86_64 通过 QEMU 10 验证兼容性；仿真耗时不作正式性能预测。

scripts/dev.ps1 提供 Status、Build、Test、Image、ContainerTest、RuntimeImage、RuntimeTest；详情见 [环境说明](docs/environment.md)。

其他 Linux 环境：

~~~sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
MVDB_TEST_DIR=/data/core-tests ctest --test-dir build --output-on-failure
./build/minivectordb --protocol jsonl --data-dir /data
~~~

测试使用 Python 3 和 C++20 编译器。Dockerfile 的 test 目标运行验收测试，最终 runtime 目标直接启动数据库前台进程；运行不需要联网。数据库、临时排序文件和大结果的临时序列化文件均放在 /data。默认 JSONL，开发用 TSV 须显式选择，详见 [输入输出说明](docs/dev-protocol.md)和[官方协议复核](docs/admission-protocol-review.md)。

## 验证与剩余工作

以下段落是上一轮基于 `8a67457f` 的历史 T/B/M 对照，版本范围保持不变。[最终资源对照](bench/results/resources-20261111/comparison-summary.json)记录两类过滤各三轮10000行测试，以及各两轮30000行超容量测试。10000行批量写入T下降约10.5%–11.2%，物理I/O下降48.7%–50.5%，峰值内存下降34.5%–41.8%；查询T下降约2.4%–5.0%。超容量写入T下降约10%–11%，B下降约53%–55%、M下降约49%–53%；BIGINT有索引查询T慢约0.57%，不宣称所有场景都加速。ARM64与仿真x86_64各47/47回归、20/20官方公开用例通过；最终60会话、25600计时查询及重启核验通过。中间方案的单轮诊断、无显著T收益和测试修正均保留。这些仍是本地结果，不是正式7000/40分。

按用户确认重新生成 `agent/conversation.json`，前置 `submission_summary` 和内部引用，保留历史记录及根目录同内容副本。关键指标直接写在JSON字符串内，区分用户原话、工具结果和Agent分析。摘要不是平台评分报告；正式Prompt无效的具体原因仍待平台确认。

上一候选的[三版本对照及边界结果](bench/results/validated-replay-20261111/comparison-summary.json)保留逐轮数据与中位数：相对 a7c88df6，10000 行字符串混合查询快约 2.63–2.66 倍，BIGINT 查询两次对照慢约 2.2%–4.5%，I/O 基本不变；30000 行超出完整快照容量后基本无收益。该轮ARM64 与仿真 x86_64 各 43/43 回归、20/20 官方公开用例通过；7 组实验合计 114 会话、61600 条计时查询及重启核验通过，另保留首次大结果超限的失败实验。实验器改用分页回查大结果，不放宽官方响应上限；上述计数保留当时版本范围。

[此前协议与评分测量复核](docs/submission-contract-results.md)记录 a7 发布前的检查及混合过滤修复；[此前得分优化记录](docs/score-first-results.md)、[准入修复记录](docs/admission-validation.md)与[历史验证](docs/validation.md)保留各自当时的版本范围。基线正式通过不能代替当前改动的准入验证，也不能由本地耗时直接换算正式 E 或排名。独立测试包括随机参考模型、语句原子性、跨表事务、强制终止恢复、强制外排及真实写盘失败。

公开协议检查器及功能用例按上游 `f4d4e266` 原样保存在 `bench/vendor/seekdb-round2-f4d4e26`，逐文件身份见 `upstream-lock.json`。`bench/run_contract_benchmark.py` 使用其传输层，分开测量批量写入和 TopK，支持整数与字符串过滤；它是本地对照工具，公开仓库未提供本赛题的官方性能基准。`scripts/check-submission-contract.py` 可对工作区或指定完整提交检查材料路径和格式；它不评估 Prompt 分数。

本项目的 `.github/workflows/minivectordb-round2.yml` 验证材料完整性、官方题包身份、原生 x86 构建、内部回归和公开协议。它不运行隐藏准入、不提交比赛、不生成正式分数。上游遗留流程继续保留。

页面校验使用 IEEE CRC32 slicing-by-8，字节算法保留作独立对照。格式04保留03的数据页打包与溢出记录规则；每次扫描的 packed-page 临时缓存默认关闭（`packed_scan_cache_bytes=0`）。本轮改变了新建文件的节点存储格式，保留旧文件读取和原提交同步策略。VA 精确剪枝及追加日志仍为独立研究原型。详见[格式说明](docs/storage-format03.md)、[当前数据缓存契约](docs/data-buffer.md)及记录旧16 MiB配置的[历史缓存结果](docs/buffer-results.md)。

## 论文与学习材料

- [按得分项重排的查询、内存与提交优化](docs/score-first-results.md)
- [上传版0e954的真实JSONL性能基线](docs/jsonl-baseline-0e954.md)
- [同步扫描优化、负面结果和验证](docs/paired-scan-results.md)
- [有界扫描快照、数值修复与交错读写实验](docs/scan-snapshot-results.md)
- [旧版正式失败复核与版本区别](docs/reported-admission-failure.md)
- [文本校验优化、负面实验与学习证据](docs/text-validation-results.md)
- [按收益排序的行 CPU 优化、真实旧新对照与回归](docs/row-cpu-results.md)
- [刁钻测试、提交确认漏洞修复与故障注入](docs/adversarial-results.md)
- [跨查询缓存优化与超容量对照](docs/buffer-results.md)
- [紧凑页生产集成与独立日志实验](docs/compact-results.md)
- [缓存、CRC 与紧凑页/VA 实验](docs/portfolio-results.md)
- [实验方法和限制](docs/portfolio-method.md)
- [耗时拆分与D盘路径对照](docs/path-profile-results.md)
- [页面校验优化](docs/checksum-results.md)
- [优化对照结果](docs/optimization-results.md)
- [实验方法](bench/optimization-method.md)
- [SQL 执行研究](docs/sql-execution-research.md)
- [向量与存储文献](docs/performance-research.md)
- [局部扫描基准](bench/README.md)
- [真实学习记录](agent/conversation.json)

agent/conversation.json 为材料入口，根目录 conversation.json 保留同内容兼容副本。材料区分用户、Agent 和工具来源，不把 Agent 的实现或解释冒充用户理解，不包含令牌。本地文件哈希摘要不等于平台评审输出中的“材料摘要”，本项目不自行生成正式分数或换算评分。

历史上曾对照平台显示成功的 `8b7054`：两版各公开20/20、95SQL，58条协议响应仅一条排序修复差异；详见[成功版本兼容性复核](docs/successful-version-compatibility.md)。该报告保留当时上传前的证据，不代表当前版本再次通过。已绑定版本的最新正式反馈属于 `aea69813`：MySQL 通过、E=1630.670965、Prompt 合并无效。用户随后报告一次 JSON response timeout、未执行性能评测，但尚未给出该次 commit，不能归因到本候选；Prompt 是否被同时跳过也未知。公开检查不等同隐藏准入。

上一轮的[无索引快照、缓冲复用与临时向量优化实验](docs/unindexed-results.md)保留当时三轮迭代、负面结果、38项回归与真实 JSONL 对照，不作为本轮最终验证结论。
