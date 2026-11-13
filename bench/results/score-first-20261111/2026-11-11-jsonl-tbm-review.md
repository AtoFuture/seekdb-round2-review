# JSONL 对照的 T/B/M 口径审查

2026-11-11，Asia/Shanghai。只读审查 `repo/bench/run_jsonl_baseline.py`、`run_jsonl_versions.py`、公开传输 Session，以及既有 JSONL 原始资源快照；没有运行 benchmark，也没有修改驱动或生产代码。最终新版本结果仍在生成，本文不填新结果。

## 时间 T

- `write_ms` 是100条一批的各次请求往返耗时之和，计入主机JSON编码、管道/Docker传输、数据库执行和持久化、响应接收/解析；不计建表、索引构建、数据/真值生成及两次请求间的主机检查开销。
- `query_ms` 是1000次请求往返耗时之和，包含结果输出与主机解析；不计10次统一预热、主机正确性比较、后续4条ANN附加检查与重启核查。不是完整循环起止墙钟，也不是服务器纯CPU时间。
- 正确性比较虽在单条计时外，仍穿插在请求之间，会改变请求间隔、后台写回机会及热状态；同驱动A/B可比较，但不可说等同平台独立计分窗口。报告写明“主机测量的请求耗时之和”。

## 峰值内存 M

每个样本创建新容器，首次运行期间没有重置 `memory.peak`。`resources_after_queries.memory.peak` 覆盖该cgroup自创建以来直到查询结束采样的峰值，包含启动、装载、建索引和预热；不只覆盖查询，也不是RSS。其后的ANN附加检查与重启未计入该快照，符合把后续核查排除的意图。

最初驱动没有 `resources_after_write`，indexed的 `resources_after_load` 已含建索引，不能叫纯写入M。主代理收到初审后增加了写入完成、建索引之前的 `resources_after_write`；新结果可用该点给出写入窗口M。此前旧结果只能用unindexed的after_load近似同范围，indexed旧记录的纯写入B/M不可追补猜测。

`run_jsonl_versions.py` 目前的 `peak_bytes` 是各轮after_queries峰值的中位数。可以称“重复运行的峰值中位数”，不能称三轮所有瞬间的最大值。新报告分别汇总after_write和after_queries；禁止将两个peak值相减当作某阶段内存峰值。

## B 的诚实汇总方案

Linux定义 `io.stat` 以major:minor为键，rbytes/wbytes分别为读取/写入字节；`memory.peak` 包含cgroup及其后代。文件系统需支持cgroup写回归属。这些基础语义见[内核官方文档](https://docs.kernel.org/admin-guide/cgroup-v2.html)。

对每个完整样本解析原始 `io.stat` 为设备字典，保留rbytes与wbytes，按设备分别计算：

1. **写入观测B[d] = after_write.rbytes[d] + after_write.wbytes[d]。**每样本新cgroup，绝对累计值包括启动和建表；若报告ready→after_write增量，须另名“就绪后增量”，因为它扣掉了启动I/O。
2. **查询项目观测B[d] = after_queries.rbytes[d] + after_queries.wbytes[d]。**涵盖此前启动/装载/建索引/预热；不能用after_queries−before_queries替代整个项目B。
3. **纯计时查询区间增量ΔB[d] = after_queries[d]−before_queries[d]** 可作为附加诊断。它可以为0，但0只表示该区间未增加归属到该设备的块I/O，不表示数据库从未读取记录或整个查询项目零I/O。
4. 每轮先按同一设备求rbytes+wbytes，再对各轮B取中位数；不可分别取r/w最优值拼接。文件大小 `database_bytes` 单独报告容量，不参与B替代。dbytes/rios/wios不是本题定义的读写字节和。

读失败或字段为null应报告不可用，不当0。只有成功读取的完整io.stat中某设备尚未出现，才可将该快照该设备起始计数解释为0。设备标识/计数异常或出现负增量时应报告异常，不能截成0。

### 设备不能盲加

当前审查的上一轮完整样本只出现单个 `8:48`，没有在这些记录中发现跨设备重复相加；原驱动只保存字符串，没有验证设备拓扑或/data映射。主代理随后在新驱动补充了由st_dev得到的 `data_device` 以及 `memory.stat`、`memory.events`。正式新记录仍需按实际设备检查拓扑，不能硬编码8:48。

建议用 `os.stat(s.bind_data.name).st_dev` 与major/minor记录/data文件系统设备，并检查是否有dm/分区/底层设备叠加。多个major:minor并不自动证明相互独立；没有拓扑依据时逐设备列出，不将不同层次计数相加。不要合计父子cgroup；本驱动读取的是单个容器cgroup。

数据目录是 `/var/lib/minivectordb-jsonl-bench` 下的独立目录，不是独立块设备。若它与容器镜像/运行库共用设备，cgroup该设备累计可能同时包含启动加载等I/O，不能从io.stat按文件名拆开。因此推荐名称为“容器在设备d上截至采样的块I/O观测值”，不宣称隔离数据设备的官方B。

### 结束写回与冷启动边界

最后一次资源快照在附加检查、shutdown之前。传输层shutdown随后会删除容器，没有保留cgroup等候所有相关写回完成后的第二次快照。提交协议已有同步落盘，可支撑应用提交语义；但仅凭这些即时计数，仍不能证明目录、临时输出、后台归属等所有相关写回均已统计完毕。

可追加只读采样 `memory.stat` 的file_dirty/file_writeback、memory.events和cgroup身份，暴露未完成写回/OOM情况；它们是诊断，不能取代统一完成写回口径。本轮不需要修改全局配置、sync/dropcache或人为等待来把现有结果包装成官方B。

新数据目录不等于统一冷页缓存。驱动未显式统一系统页缓存状态，加载过的镜像页可能复用；旧记录有的启动rbytes非零、有的为0，应保留而非筛除。版本和indexed/unindexed轮换有助减小顺序偏差，不能消除所有环境影响。WSL的禁用swap仍不可强制验证，保留原限制说明。

## 最终报告建议

每个版本和索引模式分别列：写入请求总T、写入截止累计设备B、写入截止cgroup峰值M；查询请求总T、查询截止累计设备B、整段cgroup峰值M，并给三轮原始值/中位数。索引准备时间和数据库文件大小放附列，范围文字贴近表格。

允许结论为“同环境下T下降、观测B持平、峰值M变化多少”；不将局部查询ΔB=0当作总B=0，不把无索引/有索引和不同轮的最佳值拼成一个版本，不根据未知平台基线和公式生成E或正式排名预测。
