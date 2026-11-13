# MiniVectorDB

OceanBase 加赛提交分支为 `2026-competition-round2`。本项目基于原有实现继续优化，最新已收到的正式结果对应 `ff117199f8077de7a6e3e1f91a611397bb6641a0`：MySQL 准入通过，性能原始 E=2576.778405，Prompt 返回合并无效/待复核提示。此后的原生测量均为自行生成的数据，不能换算官方分数。

## 运行与接口

Dockerfile 最终 runtime 目标以前台进程启动 JSONL，固定使用 `/data` 保存数据库和临时文件；运行不需要联网。启动输出 ready，每个请求返回一条完整 JSON，附换行并 flush。错误响应不混入部分成功结果，诊断不写 stdout。

支持 CREATE TABLE、INSERT、多行 VALUES、SELECT、UPDATE、DELETE、CREATE VECTOR INDEX、BEGIN/COMMIT/ROLLBACK；类型为 BIGINT、VARCHAR(n)、VECTOR(n)，单 BIGINT 主键。支持普通列比较及 AND、多列排序、LIMIT、L2_DISTANCE 与精确 TopK。exact/ann 都返回精确结果；并列按主键排序。完整语法以题包为准，不支持的语法报错。

## 本轮实现

新建 CLI 数据库采用 MVDBJL08 追加记录存储：不可变记录链、双校验提交根、内存键到偏移目录；旧 02–07 格式自动识别并沿用原后端。每批全部写完后才发布内存变化，提交 fsync 成功后才确认。回滚改变逻辑可达性，不截断扫描快照仍引用的记录。格式见 [journal-format08.txt](docs/journal-format08.txt)。尚无历史日志回收；目录与恢复成本随记录规模增长，不能把有界读缓存误称为全库固定内存。

默认 ProcessCrash 提交策略针对进程强杀后内核和文件系统继续存活的模型；Ordered 另加数据先同步的屏障。提交 I/O 失败后须重新打开。事务、保存点、故障注入、重启与旧格式测试保留。

类型化向量快照、原始行快照和投影缓存共用 64 MiB 上限；投影缓存另受 8 MiB 上限约束。预算不足时回退扫描，不发布部分缓存。非 SELECT、写入、DDL 和事务命令执行前使缓存失效。缓存只复用已验证的数据，每次仍重新计算过滤、距离、TopK；不缓存查询答案。

精确距离使用原 float32 值和规定的 double 累加顺序；AVX2/AVX-512 运行时分派保留无相应指令的回退。整数辅助数据仅用于有保守下界证明的候选排除，保留精确复算，不做有损量化。磁盘编码按原字节无损恢复。

JSONL 响应跨请求复用不超过 64 KiB 的缓冲容量。每次先清空内容并关闭旧临时文件，大结果仍在 /data 下暂存，完成序列化后再输出；换行、flush 和错误语义保持不变。

## 验证与材料

两台原生 x86_64 分别在 2 CPU、2 GiB、无交换、运行期无网络的容器中执行回归、公开协议、精确 oracle、重启和 T/B/M 对照。常规负载为 10000 行、128 维、1000 次查询、三轮；另有随机 float32/随机文本和 30000 行容量验证。失败及退步实验保留，采集异常不计作成功样本。原生结果见 [本轮证据](bench/results/release-20261112)，历史正式结果不被本地测量覆盖。

材料入口 [agent/conversation.json](agent/conversation.json) 与根目录 conversation.json 逐字节相同。文件包含真实用户记录、实际工具摘要和明确归属的 Agent 分析；没有补造用户学习过程。`agent/material-digest.json` 仅证明本地材料完整性，不是平台内部评审摘要或评分。历史完整归档保留。

本地检查：

~~~sh
python3 -B scripts/check-submission-contract.py --check-internal-references
python3 -B scripts/check-learning-material.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
MVDB_TEST_DIR=/data/core-tests ctest --test-dir build --output-on-failure
~~~

公开题包固定在 `bench/vendor/seekdb-round2-f4d4e26`，2026-11-12 再核对上游 STDIO_PROTOCOL.md 内容一致。公开测试通过不能代替正式隐藏准入，也不能保证 Prompt 得分。
