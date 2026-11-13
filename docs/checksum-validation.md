# 页面校验优化验证

日期2026-11-09；核心提交6f1824f48673fea71da59d9a20719bd25e4fe12c，基线4ad8b99326cbd24523912143bff58b2e286a3c78。全部在D盘，不推送、不提测。

- 实现者本地16/16测试通过65.55秒。新增测试初始RED为内部头文件尚不存在引发编译失败，没有伪称运行时失败。
- 最终test容器16/16通过65.19秒；runtime独立容器启动/重启检查通过，包含持久化与EOF回滚。
- test镜像sha256:ec37ff7b66dca3016bc21cfc14f0ced5a56747b5cc36ba427d15ea09cda8bac2。
- runtime镜像sha256:2680aa18f52ae4f62c989d49788225e3ac0eba6e9276fe71bb46477a9d1d9438。
- 独立规格审查通过，另执行core_checksum 1/1，0.55秒。独立质量审查通过，无阻塞问题；报告归档在docs/reviews/crc-quality-review.md。
- CRC参考为独立逐位分支实现；覆盖4096每个字节位置、256随机页、固定模式、偏移24/32、校验字段被忽略、相邻字节变化及输入不变。新文件每页的根/分支/跨页记录校验码用参考算法核对，再重启读取。旧存储损坏检测、写失败和崩溃测试保留。
- 全部校验字段、完整页检查、提交同步和格式02未变化；未添加CRC32C硬件指令或不同多项式。

首次构建因专用Docker服务停止而失败；启动既有D盘服务后重建通过。未更改全局WSL配置，未清理其他Docker资源。当前WSL不支持swap limit，不能据本地测试宣称正式资源准入。

最终性能结果见checksum-results.md；六组使用同一旧runtime镜像并挂载各自二进制以控制环境，最终新runtime镜像另行验收。容器测试与基准顺序运行，没有并行项目测试负载。

本地日志D:/Codex/MiniVectorDB/work/crc-{red,build,tests,image,container,runtime-image,runtime}.log及checksum-comparison.log。具体性能原始样本在bench/results/checksum-*；不估算官方分数。