# 2026-11-12 评分点迭代证据

以正式aea69813（1630.670965）为基线，逐版对照包含stage1至stage10；stage11链接期优化构建失败，未采用。当前运行代码身份见stage10-source-binding.json，早期candidate-source-binding.json属于stage5。

最终完整中位数与所有样本在stage10-native.json、final-comparison.json和stage10-raw-results.tar.xz。第7–10轮每份raw-results归档保存对应两台原生主机的构建、CPU、镜像、公开用例、逐轮计时/cgroup采样、独立真值与重启核验文件；仅去除重复且可再生的oracle-input.bin。原始ZIP及省略输入的SHA256、字节数见raw-artifact-manifest.json；生成器、种子与SQL工作负载哈希均保留。归档未删除任何退步样本。所有8份原始comparison.json与提交摘要逐项一致。

最终原生测试提交c09d76bf0919c8a2cbce8835f021a622f65fa26b，run43130504071。两台均AMD EPYC9V74，第一台不暴露AVX512F，第二台暴露；不根据型号推断特性。各49/49内部、20/20公开，标准144会话/96000计时查询和超容量24会话/1600查询通过。ARM对应final-stage10-arm.tar.gz。

第十轮对第九轮：无AVX512主机查询近似，个别约0.1%–0.6%退步；有AVX512主机查询约改善3%–4%。核心API微基准约10%的收益不能当作完整JSONL收益。第十轮第二台写入存在显著波动：VARCHAR相对前版中位数退步36.67%，原始轮次完整保留。

本地计数保留启动、加载、索引构建、预热与写回窗口，正确性校验在采样后进行；延长drain上限对两版相同，不少计I/O。全部为自生成数据和共享主机，不是官方T/B/M或E。用户最新JSON超时反馈未提供commit，隐藏失败尚未定位；公开用例通过不等于已修好隐藏失败。Prompt路径/格式通过不等于平台有效或40分。
