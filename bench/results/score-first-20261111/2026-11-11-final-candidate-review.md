# 最终候选只读独立复审：typed cache、validated kernel、提交策略

日期：2026-11-11。本次仅静态检查生产路径、独立测试与相对 524133 的存储改动；不编译、不执行测试、不跑性能。统一 GREEN 及最终镜像验证由主 Agent 执行，本报告不是通过凭证。

## 审查结论

当前检查未发现阻断生产正确性的实现问题。关键条件是维持单线程/单写者及无外部改写约定，且 ProcessCrash 的保证仅覆盖此前报告限定的纯进程强杀模型。此前提出的真实 packed 页与同 fd 测试缺口已修正。

首次采样观察到的默认值（后续启用复核见下节）：typed_scan_cache=true、data_cache_bytes=4MiB、Database commit_sync_policy=ProcessCrash；底层 Store 默认 Ordered。**首次采样时 validated_vector_kernel 为 false**，不能把 benchmark 的 optimized 模式（设置该开关为1）直接描述成当前默认运行镜像。

## 私有 validated kernel 的有限性证明与边界

检查位置：src/vector/validated.hpp、src/vector/exact.cpp；src/sql/executor.cpp:295 附近。

公开 l2_distance 与 exact_top_k 仍走逐分量检查。私有入口只取消已经证明有限的候选和查询在距离循环内的重复有限性检查；入口依旧检查完整查询向量，每个被访问候选仍检查维度。减法转换为 double、逐维累加、最终 sqrt、按返回距离和有符号主键排序的表达式与 checked 路径相同；没有引入 fast-math 或近似距离。

生产 SQL 的候选来源逐一成立：

- 未缓存无索引/主键路径：SelectRow 在谓词执行前构造；eager decode_row 和 lazy LazyRowView 都校验全部向量长度及有限性。scratch 只从该已验证行复制位模式。
- 冷索引路径：read_index_vector 检查长度及有限性，还与完整验证的 base row 比较；局部 vector 在同步 visitor 返回前存活。
- 暖 raw snapshot：仍通过 SelectRow 再验证，再提供候选。
- 暖 typed cache：只由上述完整验证的扫描构建；持有自有对齐 float 数组，build 完成才 ready，所有可能改变数据库视图的语句都会先失效；visitor 同步返回，外层 busy guard 拒绝重入。
- query 来源于本次语句的绑定结果，SQL 内部 source 不修改它，也没有在候选遍历时调用用户输出 callback，因此入口一次检查在此次调用中有效。

错误路径保留维度异常、非法 query、source 异常传播。私有入口不是任意未验证输入的安全 API；其前提不能迁移到公网/公共向量接口。非阻断文档建议：validated.hpp 可进一步明确 query 与候选都必须在整个调用期间保持原值，防止未来调用方通过别名修改 query，使一次入口检查失去证明。

现 tests/validated_vector_tests.cpp 提供独立全排序/算术 oracle、随机有限位模式、极值、次正规数、正负零、sqrt 舍入等距、主键规则、非法查询与维度、公开接口仍拒绝 NaN/Inf 的边界；检查的是有意义的数值与错误契约。但这不等同于 SQL 分支覆盖。

**启用为默认之前的验证要求：** 显式 validated_vector_kernel=true，覆盖 typed 开/关、冷/暖索引、索引关闭、eager/lazy、scratch 开/关、主键点查、非法存储向量与错误查询。当前 typed_scan_tests 的 settings 没有显式打开该开关，默认仍 false；如果随后默认改 true，需重新执行这些测试和最终运行入口验证，而不是只引用此前 checked 默认的通过记录。

## typed cache 有效性与内存

检查位置：src/sql/typed_scan_cache.hpp 与 executor.cpp 的 typed_source/execute。

- 唯一身份包含 table、index、vector column；解析成功的非 SELECT 在语义执行前失效，覆盖失败 DML/DDL、BEGIN、COMMIT、ROLLBACK。解析失败没有修改数据库，因此保留缓存不导致过期视图。
- 缓存构建保存所有行的向量及 BIGINT 列，再评估当前过滤条件；零匹配构建可以正确服务后续不同过滤。仅 BIGINT 谓词使用快路径，其余条件清理 typed 后回到原路径。
- ready 只在扫描完整结束时发布。超容量 release/refuse，不发布前缀；异常 clear，partial append 的 count 不增加。无索引构建仍经过全部行验证；有索引构建仍验证两边键集合及每个向量一致性。
- shape/budget 变化先释放；同形状数组复用时会重建列映射和主键槽。invalidate 清空可见行数和身份，不读取旧内容。
- typed eligibility 成立时先 clear raw；进入 raw、一般 SELECT 和外部排序前清理 typed。两种扫描表示不会同时持有8MiB预算。
- 数组长度先用除法/减法校验，再计算乘积，且 budget 截断到8MiB。vector/scalar/列映射三组数组的逻辑分配总和不超过预算，bad_alloc 退回流式处理。不存在由缓存依赖全库装入才能返回正确结果的路径。

非阻断限制：缓存数组预算不是进程 RSS 上限。默认缓存数组合计上限约1MiB索引+4MiB数据+8MiB扫描表示，此外还有 allocator、查询向量/scratch、TopK、SQL文本和解析对象、结果与内核页缓存。失效时可保留容量，所以写入阶段也可能保留此前缓存的内存；应以最终容器峰值为准。缓存内不可变数据复用意味着不再逐查询重新验证全部原始行字节；外部改写、真实媒体损坏不在现有 Store 契约中。

## ProcessCrash 实际实现与测试

store.cpp:347 在 open 前拒绝无效 enum；创建数据文件和父目录同步保持；commit:482 仅按策略条件化根发布前的同步，根 pwrite 与最终 fsync 均使用 s.fd，最终同步成功才更新 generation/durable_root/active。任意提交异常仍 poisoned；SQL 层仍拒绝回滚不确定的提交。

这符合 outputs/2026-11-11-commit-policy-review.md 的限定证明，不扩大为断电或真实 EIO 后总能全旧/全新。JSONL 路径先等待 Database::execute 返回，再完成/发送响应，没有提前发送成功。真实输出失败仍须按既有协议行为处理，独立 Store ACK 管道测试不能替代这一验证。

commit_policy_tests.cpp 现在 batch 包含一个9000字节 overflow 值与两条小值，格式03实际包含 packed 页；wrapper 记录 commit_fd 并约束根写/同步同 fd。两格式、双根槽、短写、EINTR、fsync 调用前/后注入 EIO 与 poison 检查存在。

SIGKILL 由父子管道握手与 SIGSTOP 确定边界，不依赖 sleep 猜时；父进程 waitpid 确认被 SIGKILL 并回收后再重开。验证三个命名空间独立构造的全旧/全新 oracle，并跨策略反复重开，不以一次查询结果代替完整事务检查。

无额外生产修改建议；保留未验证的故障模型边界和最终统一 GREEN 门槛。

## 审查时文件摘要

- src/storage/store.cpp: 1fe62d764d897e6d0f20f4d1213efc121aadd4be8ea3df8079b7638cf43785f9
- src/sql/executor.cpp: 3d2e7299a040f26eb01b7ffa7d8f70e5a6250d1c87682c2bee1ec307886ae372
- src/sql/typed_scan_cache.hpp: 79dd132152dcbfa2f4dca17697f6495d168d572f5e0653b0c527f0d109a437b2
- src/vector/exact.cpp: b67dc037b1b6bca39b14c00a43a96f1a5a0aa148b49b9fd82a55efb94a74359d
- src/vector/validated.hpp: 975ebf73084b48af788d3fe188ddac757b369a9c590d48929e6d6ca105a89bb9
- include/minivectordb/database.hpp: b115566f1be0288ed62f2642aadd5ab5cde6853bcd04abfb68f2234118deaf51
- tests/commit_policy_tests.cpp: c05a1947054fb8dba5d3195362ab52ee88108b82eb843d2421639672e2401ad2

这些摘要只绑定本次读到的状态；后续改变默认值、数值实现或候选来源应复核受影响部分并绑定最终验证版本。
## 后续默认启用的复核

收到主 Agent 的启用通知后再次只读核对：validated_vector_kernel 已为 true；typed=true、data cache=4MiB、Database ProcessCrash 保持。database.hpp 新 SHA256 为 32e4145b640dab0459369b86832ab3f1ed170e9be71b9c5123acd781767901ba。executor.cpp、exact.cpp、typed_scan_cache.hpp 的摘要仍与上面一致，因此候选有限性、生命周期和错误路径证明未改变。

上述默认false的观察仅记录首次审查状态，不是当前候选配置。主 Agent 计划以最终true默认运行完整Docker及SQL来源/损坏矩阵、public/model/runtime/JSONL；本审查没有执行这些检查，等待对应结果，不将计划冒充GREEN。默认启用本身没有引入新的静态阻断项。