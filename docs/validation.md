# MiniVectorDB 验证记录

最新阶段：[刁钻测试与提交确认修复](adversarial-results.md)，36/36 全量容器测试、加强断言后的3/3专项、公开20/20、1,344次SQL模型检查和475组存储故障案例通过，真实 JSONL 旧新对照12,000次查询通过。保留前轮行CPU优化；尚未推送或正式提测。下文保留历史阶段记录。

上一阶段：[耗时拆分与路径对照](path-profile-results.md)，17/17容器测试65.68秒，12组实验全部通过。

最新增量：[页面校验优化验证](checksum-validation.md)。以下保留上一轮按需解码/批量写入记录。

日期：2026-11-09。本轮最终核心提交 8f1a679ff0c4d59e13949e228cc5f9f198698578。测试之后只有基准、文档及学习材料归档，不改变核心代码。前一轮原始记录保留在 [SQL 里程碑验证](validation-sql-milestone.md)。

## 最终容器与运行结果

- test 镜像：sha256:eea97e0a038e39cee02b688144bce1c352fe63ed1c629a4adad9ab1d3dab5c3a。
- runtime 镜像：sha256:ccbffecd48173eaadd19951ecb43fba7c5ae003c9dd5a5c82648fac2a1504cb6。
- 最终 test 镜像 15/15 CTest 通过，总计 77.02 秒。包含 types、vector、storage、storage_failure、namespaces、codec、sql、sort、sql_crash、sql_edges、sql_failure、sql_batch、cli、selective、batches；cli 内含七项黑盒测试。
- runtime 在只读根目录、无网络、2 CPU/512 MiB 条件下，两次独立启动复用 /data，验证前台入口、提交持久化、EOF 回滚和重启向量排序，通过。
- /data 和构建/镜像所在 WSL 虚拟磁盘均在 D 盘。全局 WSL 配置保持不变。宿主不支持 swap limit，不能声称等同正式资源准入。

## 新增覆盖

原有 CRUD、跨表事务、真实写盘失败、随机参考模型、外排、收到确认后 SIGKILL 恢复等覆盖保留。本轮新增：

- 完整/按需解码四组合结果对照；点查、扫描、过滤 TopK、投影和强制外排；被过滤或未投影字段的损坏、非有限向量、逐字节截断仍报错。
- 按需构造计数、缓存身份和 float32 精确位；正负零、次正规数、最大有限值；NaN/正负无穷在访问字段前拒绝。
- 随机批次、极端有符号键、命名空间隔离、空/不存在删除、重复键和大小超限在追加前拒绝；单条超大记录处理。
- 保存点恢复、更早批次撤销、300 行 UPDATE/DELETE 固定旧根扫描、提交/回滚/重启；300 KB/600 KB 记录越过分块字节边界。
- RLIMIT_FSIZE 真实写失败：裸批次失败无需外部 restore 仍保留旧根；SQL 第一个 128 行块成功后后续失败，整条语句撤销且先前事务语句可提交。
- 实现者另用旧 7f3c4ac 的读取器读取被新批量写入器扩展的格式 02 文件，验证 101 行及其他命名空间。此项不在 CTest 中，独立审查者未重复执行。

## 独立审查

[规格审查](reviews/optimization-spec-review.md)：PASS。独立 262 行四组合参考对照，含主键不在首列、有符号极值、外排、多块更新/删除、重复回滚和重启；四组重点测试 4/4，通过 22.48 秒。位复制修正后 codec/selective 2/2，通过 4.40 秒。

[质量审查](reviews/optimization-quality-review.md)：PASS。审查回调生命周期、整行验证、计数、批次界限、异常安全、不可变快照和同步顺序；独立 selective/batches 2/2，通过 6.39 秒。后续复查位复制修正及容器超时清理，无阻塞发现。

基准实际一秒超时检查通过：只删除本次拥有的容器及其临时数据目录，没有遗留负载。该检查针对父进程捕获超时的正常清理路径，不声称能抵御宿主或 runner 被强杀的所有情形。

## 性能与剩余限制

[完整对照报告](optimization-results.md)保留初版负面结果和最终复测。最终 SQL 四模式三轮，共 480 次独立精确查询与 12 次重启全行核对通过；解码专项 30 个样本配对校验一致。不同版本不混算。

尚无正式协议、错误码和测试题包；本地 dev-tsv 不代表官方兼容性。尚未实现 ANN、页缓存、空间回收；单键修改仍有 16 层路径复制。排序预算不是总 RSS 硬上限，进程强杀可能留下孤儿排序目录。测试证明范围为进程终止恢复，不是断电/磁盘损坏保证；首次目录同步失败后重开未重试目录同步的旧健壮性缺口未扩展。直接构建依赖锁定，传递依赖未完整离线归档。

学习材料区分用户、Agent 和工具，不将 Agent 的实现或解释当作用户已经掌握的证据。本轮无推送、无正式提测、无初赛分支修改。

原始日志均在 D:/Codex/MiniVectorDB/work/：optimization-v2-image-build.log、optimization-v2-container-tests.log、optimization-v2-runtime-build.log、optimization-v2-runtime-test.log、optimization-main-run-v2.log、decode-experiment-v2.log。完整审查报告及独立探针也保留在该目录。

本轮已对照平台显示成功的 `8b7054`：两版各公开20/20、95SQL，58条协议响应仅一条排序修复差异；详见[成功版本兼容性复核](successful-version-compatibility.md)。该报告记录上传前验证；最终版本以提交SHA为准，平台两项分数仍未提供。
