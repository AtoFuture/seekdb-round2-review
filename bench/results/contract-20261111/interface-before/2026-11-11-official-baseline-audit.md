# 官方基准使用情况独立审计（2026-11-11）

## 结论

已认真使用官方公开功能题包和JSONL通信驱动，也按题面默认规模设计了端到端性能对照；没有找到被漏用的本赛题公开性能驱动、参考数据库或冻结基线数值。现有本地性能结果仍不是官方benchmark成绩。它能说明同环境新旧镜像的差异，不能解释正式E=856.552980的计算过程，更不能预测名次。

本次只读核查上游、题面缓存及现有本地脚本，未启动计时实验、未改生产代码、未提交/推送/提测。仅报告和公开核查证据写入D盘outputs。

## 核查范围与直接证据

1. GitHub插件读取官方oceanbase/seekdb的f4d4e26653af823a7601668802f80e374ad6643c完整递归tree：102项，truncated=false。该tree没有performance/benchmark/baseline目录或数据库参考实现；Dockerfile、conversation.json均为0字节占位，根README不存在。公开应用内容为STDIO_PROTOCOL.md、mysqltest和protocol_check，其余主要是原seekdb配置/CI遗留。
2. protocol_check/README.md第4行明确本目录“不包含隐藏测试、基线、评分、平台凭据或回执接口”。run.py只做Session启动、逐条SQL断言、正常关闭和结果报告；duration_ms含启动、关闭和清理，不是T/B/M性能得分。最终report.note再次明确no performance/Agent scores。
3. mysqltest/README.md明确20公开样例、95SQL。完整准入29例，额外9例输入保留；本包不含数据库实现。公开例通过不等于性能或Agent评审通过。现有项目公开20/20及独立检查已使用这些资源。
4. protocol_check/config.json为2 CPU、2GiB、128 PID、启动120秒、单请求60秒、case600秒、响应上限16MiB、abs/rel容差1e-5。该配置没有性能数据生成器、固定基线T/B/M、I/O带宽/准备时间/缓存流程。
5. .akperformancetest只有sysbench_obp、bmsqltest_obp、sysbench_prepare、obstresslocality、bigtrx五个任务名；不是MiniVectorDB负载定义，也没有实现这些任务的脚本/config。
6. .github/workflows/seekdb.yml主要对master/develop触发，运行build_release/src/observer/seekdb及obclient/mysqltest；是原seekdb引擎CI。当前第二轮tree连它所依赖的build.sh/deps/test目录也不存在。compile.yml为通用编译；.gitlab-ci.yml只路由test/ci/templates/下模板，该目录也不在当前tree。
7. .github/workflows/vldb-benchmark-comment.yml只消费VLDB CI的vldb-fts-large-benchmark artifact，将全文检索和Document AI分数发成PR评论；不是MiniVectorDB性能运行器。相应VLDB CI workflow不在本分支。
8. juanzhi-gcb/powercontext的master完整tree为3326项，无本赛题路径；evaluation/performance仅README，evaluation总README标该类None yet。2026-competition/competition分支完整tree2335项，competition/baseline.toml冻结的是PowerContext v1.2.0源码9db1753434162c0aa7c9f1189a1722e617880a52，README的11题总240分涉及CSV、PDF、上下文、Embedding等另一比赛题集。2026-competition-ecosystem tree309项为其他生态项目证据。不能将这些baseline或benchmark替代本轮MiniVectorDB基线。

公开源链接：
- [官方完整tree](https://api.github.com/repos/oceanbase/seekdb/git/trees/f4d4e26653af823a7601668802f80e374ad6643c?recursive=1)
- [官方公开驱动说明](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/protocol_check/README.md)
- [官方资源配置](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/protocol_check/config.json)
- [官方mysqltest说明](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/mysqltest/README.md)
- [旧通用性能任务名](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/.akperformancetest)
- [FTS结果评论工作流](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/.github/workflows/vldb-benchmark-comment.yml)
- [PowerContext另赛题说明](https://github.com/juanzhi-gcb/powercontext/blob/7065787f39f265f44d683d00c657669230ae4879/competition/README.md)

## 已有性能实验和题面的对应关系

审计读取repo/bench/run_jsonl_baseline.py与docs/score-first-results.md。对照不是完全随意设计：

| 项目 | 已有实测 | 题面3.3默认/规定 | 判断 |
|---|---|---|---|
| 记录/维度/查询/K | 10000/128/1000/10 | 同值 | 已按题面 |
| 批量写入 | 100条/批自动提交 | 100条/批提交 | 已按题面 |
| 过滤查询比例 | 一半 | 一半等值过滤 | 已按题面 |
| 运行入口 | 最终镜像+官方Session JSONL | 题包JSONL | 已实际复用 |
| 资源 | 2CPU/2GiB、data落专用D盘WSL设备 | 题包config同配额 | 主配额对应 |
| correctness | 独立全排序oracle、逐条结果校验及重启 | 需要独立真值和持久性 | 有力验证，但未复现整个正式负载 |
| 数据分布 | 随机整数[-2048,2048]/128，种子20261110 | 应冻结公开生成方法/隐藏正式种子 | 本地自定，不能冒称官方生成器 |
| 过滤/正文 | 10%选择率、256字符payload | 当前公开题包未提供冻结值 | 本地自定 |
| 预热 | 10次 | 题面统一预热/缓存初始化 | 未获冻结流程 |
| 请求时限 | 120秒、case14400秒 | 60秒、case600秒 | 本地放宽，不能代表正式硬门槛 |
| B | 实际data设备cgroup rbytes+wbytes累计 | 数据启动/装载/准备/预热/结束相关写回全部计入 | 未采集shutdown及写回收尾完整窗口 |
| M | 累计memory.peak | 容器整体，含装载/索引/页缓存 | 正确方向；swap不可强制禁用仍是差异 |
| T | 每次Session.request耗时求和 | 平台全部请求序列端到端时间 | 计时外逐条verify使请求之间有间隔，未完全复现连续驱动 |

现有核心C++实验不含真实JSON传输，其profile、开关消融、变形负载对定位优化有价值，不能替代实际镜像对照。现有实际JSONL对照是另外一层证据，已测出最终候选的T/M改善和B持平。两层不应混称“官方基准”。

## 评分口径

本次读取work/official-yuque-markdown.txt缓存，元数据/最新页面获取由主Agent另行复核。缓存3.3.3为各项目R=(base+epsilon)/(candidate+epsilon)，f(R)=R/(1+R)，E_item=0.6f(R_T)+0.2f(R_B)+0.2f(R_M)，E=(E_write+2E_query)/3；epsilon为1ms、1MiB、1MiB，每轮先算E再取三轮中位数。参考实现须精确扫描且满足同等正确性/持久性。

在正指标下此E范围0到1，与平台显示856.552980不同。没有权威的实际公式/缩放/冻结基线数据时，不能生成一个自称“官方E”的本地值。题面还要求公布参考实现、基线测量值、资源配置、统计工具、缓存流程及时间上限，当前公开分支未提供其中大部分内容。此处只能准确说明可获取范围，不能断言组织方不存在其他发布渠道。

## 不改生产代码的最小补验方案（未运行，待主Agent统一调度）

第一优先不是把旧C++微基准再跑一遍，而是补齐端到端观测方法。

1. 固定已正式通过524133对应runtime sha256:91fe268d07c2c0da849ef87dc00eebb96e5a9df5c2f4d9f41ac27b05ce6da856与候选sha256:8b027e8bb5e03ca61ad55304635188248ec73c9d18267b7ef4ca5c7f45264003，使用独立新容器、独立data目录；先验证两镜像同一输入oracle哈希。所有脚本、结果、临时文件仅D:/Codex/MiniVectorDB/work或outputs。
2. 保留10000×128/1000 TopK10、批100和一半过滤，先测3对交替顺序，采用官方60秒请求超时。把计时查询responses先存宿主内存，计时后统一独立校验，记录连续序列wall time及request求和两种原始量；事后完整重启内容核对。
3. B必须分离“计分阶段”和“事后检查”。在容器/cgroup仍保留时，完成相关文件同步与写回收敛采样，再停止统计；记录专用data设备各时点io.stat，而不是文件大小。如果通过docker exec做sync，需要明确辅助进程是否进入受测cgroup/M，不能不注明其成本。更稳妥是在驱动侧打开数据文件后fsync，仅用于计量收尾，且验证写回归属，不计事后检查I/O。
4. 分别报告write/query的T/B/M单轮原始值、首查询/第二次查询/热查询。缺失统计或不可禁swap明确标记，不填写0，不计算伪官方分。
5. 最小防过拟合补验：新增2个独立种子；0%/1%/10%/100%过滤命中率（含无匹配）；每三查询一次修改使typed缓存失效；30000×128超typed预算；128维主场景以外另选16/768维。先每形状一对探测，只有出现明显倒退才做三轮定位。已有核心变形实验可复用作为选择依据，不能混入官方默认成绩。
6. 等真实冻结生成器/官方基线发布或由组织方明确后直接接入，替换上述自定分布/预热；保留原本地实验为历史，禁止倒填成官方成绩。

额外最值得补齐的是官方记录的write/query各三轮T/B/M、实际E公式和展示缩放、基线冻结值与缓存初始化、性能模式是否ann及生成器。取到这些之前，可继续优化但只能承诺本地资源变化。
