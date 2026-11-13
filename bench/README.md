# 精确检索局部基准

此程序仅测内存中的距离计算与 Top10 选择，含过滤；不含 SQL、持久化、索引构建、物理 I/O 或正式容器资源统计，不能换算赛事分数。

数据：固定种子20261109，10,000条128维float32向量，1,000条查询，Top10，500条查询带通过率10%的等值标签过滤。比较全排序参考实现与流式小堆；三轮交替先后顺序，每轮在计时外核对所有1,000条结果。

~~~sh
g++ -O3 -std=c++20 -I include bench/exact_scan.cpp src/vector/exact.cpp -o /mnt/d/Codex/MiniVectorDB/build/exact-scan-bench
/mnt/d/Codex/MiniVectorDB/build/exact-scan-bench
~~~

初次记录在 results/exact-scan-before.json，GCC14.2.0、Alpine3.22.8、WSL2，未设置CPU绑核。全排序中位数783.687ms，流式堆551.126ms，局部时间比1.42198。两组都在过滤后选择TopK，不存在先取全表K再过滤的错误路径。

该数值仅说明当前程序和当前机器上的局部差异，不能外推端到端SQL或正式排名。改动内核后应保留新结果文件，不覆盖原测量。距离细微差别的精确排名以平方距离为准，欧氏距离仅在输出时开方；旧基线在候选计算阶段即开方，因此新结果需要注明实现版本差异。

最终口径一致的复测保存在 results/exact-scan-squared-reference.json：两种方案均按平方距离排名、结果输出时开方。全排序中位数765.482ms，流式堆541.186ms，时间比1.41445，三轮各1000条结果全部一致。此轮未与其他本项目测试并行运行。前面记录是开发过程探索，不用于混合计算最终成绩。
## SQL 写入与按需解码对照

新增四组合实验及独立解码专项，方法见 [optimization-method.md](optimization-method.md)，结果见 [优化报告](../docs/optimization-results.md)。v1 负面结果保留，v2 是最终修正后的同版本完整复测。

在专用 MiniVectorDB WSL 内，从 /mnt/d/Codex/MiniVectorDB/repo 执行以下命令。先完成 scripts/dev.ps1 的 Build、RuntimeImage 以及回归测试，再单独运行基准，避免竞争负载。以下示例输出使用新文件名，runner 拒绝覆盖旧测量；source 为实际构建提交，镜像应固定为实际构建的镜像 ID。核心库或源码变化后必须重新构建两个程序。

~~~sh
export TMPDIR=/mnt/d/Codex/MiniVectorDB/tmp
export PYTHONDONTWRITEBYTECODE=1
g++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -I include bench/optimization.cpp /mnt/d/Codex/MiniVectorDB/build/core/libminivectordb_core.a -o /mnt/d/Codex/MiniVectorDB/build/optimization-bench
g++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -I include bench/decode.cpp /mnt/d/Codex/MiniVectorDB/build/core/libminivectordb_core.a -o /mnt/d/Codex/MiniVectorDB/build/decode-bench
bench_source=$(git rev-parse HEAD)
bench_image=$(docker --config /var/lib/minivectordb-docker-client image inspect minivectordb:dev --format '{{.Id}}')
python3 bench/run_optimization.py --binary /mnt/d/Codex/MiniVectorDB/build/optimization-bench --output /mnt/d/Codex/MiniVectorDB/work/optimization-recheck.jsonl --source "$bench_source" --image "$bench_image" --rows 2000 --queries 40 --rounds 3
python3 bench/run_decode.py /mnt/d/Codex/MiniVectorDB/build/decode-bench /mnt/d/Codex/MiniVectorDB/work/decode-recheck.json "$bench_source" "$bench_image"
~~~

示例须保持上述顺序，两类性能实验不要并行。默认 runner 的数据和全部产物被约束在 D 盘项目内。更换其他机器需明确调整路径和环境，不能把跨环境变化混入同一对照。

## 无索引与查询临时空间实验

`run_unindexed.py` 支持参考、快照、arena复用、scratch及indexed对照，三模式轮换全部执行位置，独立精确答案和重开全行校验。源码、原始输出、一次构建成本、RSS代价和负面结果见[完整报告](../docs/unindexed-results.md)。历史`run_row_cpu.py`显式关闭新增复用，使旧CPU实验仍只改变原有两个因素。
