# 耗时拆分与两种 D 盘路径对照

日期2026-11-09，核心73bc598585d2243d52743b6b1de4336c3d35bd49，基线07e16cf8e185495a01234f26ddc48b5579b4fc67。

结论：Windows映射路径主要等待读取；Linux热查询主要消耗在页面校验。下一项选有界的已校验索引页缓存实验，主要验证写入收益。批量读取后置；本轮尚未实现缓存，没有命中率证据。

## 环境和方法

Windows根D:/Codex/MiniVectorDB/data/path-profile；Linux根为专用MiniVectorDB发行版/var/lib/minivectordb-profile。HKCU注册确认BasePath=D:/Codex/MiniVectorDB/wsl，因此后者也在D盘虚拟磁盘内。Docker data-root=/var/lib/minivectordb-docker。stat -f原始文件系统家族名称保存在summary.json。

相同二进制、镜像、种子20261109、2000行128维、每次INSERT100行自动提交、40次Top10半数10%过滤、两次预热。2CPU/512MiB、只读根、无网络。路径×统计开关×三轮交替，共12样本；无并行项目构建/测试，不清除全局缓存。临时目录和容器按本次所有权清理。

480查询与独立精确参考一致，12次重启各核对2000行；文件大小、距离次数、构造次数与结果摘要一致。wall包含等待，CPU为进程CPU；API计时排除字符串构造、参考计算和重启，包含结果回调及极小计时开销。

## 三轮中位数

| 配置 | 写入秒 | 40查询秒 | 查询CPU秒 |
|---|---:|---:|---:|
| Windows，统计关 | 5.181 | 12.790 | 1.779 |
| Linux，统计关 | 0.512 | 0.698 | 0.698 |
| Windows，统计开 | 5.115 | 12.797 | 1.830 |
| Linux，统计开 | 0.519 | 0.709 | 0.709 |

查询路径时间比约18.32，写入约10.11。这是环境差异，不是代码提速，也不代表官方Linux设备表现。后续性能实验优先D盘Linux目录，Windows路径保留回归。

主动采集开启后Linux查询中位增加约1.61%，Windows约0.05%。这是同一带统计代码二进制的on/off比较，不衡量相对旧无统计版本的全部开销；三样本不能精确分离测量成本与噪声。

## 查询leaf时间中位数（ms）

| 阶段 | Windows | Linux |
|---|---:|---:|
| pread读取调用 | 11946.808 | 81.315 |
| 页面校验 | 680.957 | 569.702 |
| 行验证 | 32.827 | 18.333 |
| 值构造 | 10.024 | 5.169 |
| 距离计算 | 6.724 | 4.260 |

Windows读取约占93.4%；Linux校验约80.3%、读取约11.5%。leaf时间是elapsed，包含统计开销，不是独立CPU归因；不得与总时间重复相加，各项中位也不保证求和等于总中位。其余含解析、结构验证、分配、堆及结果处理。

查询88,440次pread，362,250,240字节；索引7,960次、记录80,480次；校验88,440次、距离44,000次。bytes是进程读取量，不是设备物理读取。

写入32,005次读取中索引31,965、记录40；pwrite2,480、fsync40。Linux写入leaf中位：读取25.610ms、写入7.869ms、同步182.443ms、校验226.385ms。Windows对应4230.285、374.053、142.811、264.661ms。不能把同步波动作为减少持久化保证的理由。

## 下一项选择与验收

先做0（关闭）/256KiB/1MiB三档已校验索引页缓存，初始默认关闭，主要验证写入。写入99.88%的读取是索引请求，结合共享不可变树路径值得验证复用；目前没有唯一页数或重用距离，不能推断命中率，也没有证明缓存一定优于批量读取。

只保存完整验证过的索引页，记录/向量仍流式读取；明确容量边界，记录命中/未命中/淘汰。命中仍检查调用要求的页类型与层级。覆盖保存点、回滚、提交、真实写失败、地址复用、跨表、重新打开及错误页，确保缓存不返回过期数据、不绕过校验。额外元数据内存也纳入评价。

优先比较写入时间、读取/校验次数与内存，再测点查、扫描、混合更新。查询索引请求只占9.0%，查询收益有限且未证实，不能把所有CRC时间归给索引。收益不足就关闭；另可探索保持格式兼容的校验计算优化。批量读取后置，因为原生Linux热查询读取只占11.5%，不应仅为跨文件系统等待重构扫描。

## 验证与复现

最终容器17/17测试通过65.68秒，runtime独立启动/重启通过；100行4查询统计on/off冒烟一致。默认关闭、线程隔离、reset/snapshot、计数冻结、errno保存、实际成功字节数、查询结果一致性均覆盖。lazy验证与构造分开，eager row_decode为合并阶段，不重复嵌套计时。独立规格、质量审查通过，报告在docs/reviews/path-profile-*。

实现者最初缺少MVDB_TEST_DIR导致环境测试失败，正确设置后17/17通过47.39秒，过程记录work/profile-implementation.md。没有将环境失败冒充代码缺陷。

test镜像sha256:fedb9219d2405d10513ac5409567cc94a435c092a8d795106c3a444f717b26ee；
runtime镜像sha256:cab7b99c76546df33074e0cb6cb08be4f10a53e5bbb7d4b8922aa9fd94fc1324。
性能实验统一使用旧runtime镜像并覆盖挂载同一分析程序，各样本记录镜像与程序摘要；新runtime另行验收。

原始bench/results/path-profile.jsonl，汇总path-profile.summary.json；程序bench/profile.cpp；驱动bench/run_path_profile.py。
编译：g++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -I include bench/profile.cpp /mnt/d/Codex/MiniVectorDB/build/core/libminivectordb_core.a -o /mnt/d/Codex/MiniVectorDB/build/profile-bench。
在专用WSL运行python3 bench/run_path_profile.py CORE_FULL_SHA。先核验发行版磁盘确在D盘；输出拒绝覆盖，复跑需选择新输出文件。所有数据均落D盘。

仅为较小规模热查询，不是冷缓存、物理IO、正式资源准入或分数。未动初赛分支、全局设置，未推送、提测。
归档时bench/profile.cpp仅将CRLF换行为LF，原始样本的源文件摘要保持实测值；前后摘要记录在bench/results/path-profile-source-manifest.json。
