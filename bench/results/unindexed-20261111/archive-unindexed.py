from pathlib import Path
import ast, hashlib, json, re, shutil, statistics

root = Path('/mnt/d/Codex/MiniVectorDB')
repo, work = root/'repo', root/'work'
dest = repo/'bench/results/unindexed-20261111'
baseline = '64f43d1bf56af8a19eace2ddf44bae40807e6c40'
def read(p): return p.read_text(encoding='utf-8')
def load(p): return json.loads(read(p))
def write(p, s):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(s, encoding='utf-8')
def dump(p, v): write(p, json.dumps(v, ensure_ascii=False, indent=2)+'\n')
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def med(name): return load(work/(name+'.summary.json'))['medians']

internal = read(work/'unindexed-final-internal.log')
assert '100% tests passed, 0 tests failed out of 38' in internal
public = load(work/'unindexed-final-public/result.json')
model = load(work/'unindexed-final-model/result.json')
resource = load(work/'unindexed-resource-v2/result.json')
runtime_output = read(work/'unindexed-final-runtime.log').strip()
assert public['passed'] and len(public['cases']) == 20 and model['passed'] and resource['passed']
assert runtime_output.startswith('PASS runtime:')
jsonl = load(work/'jsonl-unindexed-final-summary.json')
assert jsonl['queries_verified'] == 12000 and jsonl['restarts'] == 12
assert len({m['oracle_input_sha256'] for m in jsonl['metadata']}) == 1
images = read(work/'unindexed-final-images.txt').strip().splitlines()
assert len(images) == 2 and jsonl['images']['current'] == images[1]

# Recompute medians from raw samples, including the negative experiments.
reports = []
for summary_path in sorted(work.glob('unindexed-*.summary.json')):
    raw_path = summary_path.with_name(summary_path.name.replace('.summary.json', '.jsonl'))
    if not raw_path.exists(): continue
    report = load(summary_path)
    rows = [json.loads(line) for line in read(raw_path).splitlines()]
    assert report['samples'] == len(rows)
    assert report['validated_queries'] == sum(r['validated_queries'] for r in rows)
    for case, modes in report['medians'].items():
        group = [r for r in rows if r['case'] == case]
        for key in ['result_digest', 'rows_examined', 'distances_computed', 'recovered_rows']:
            assert len({r[key] for r in group}) == 1, (raw_path.name, case, key)
        for mode, values in modes.items():
            samples = [r for r in group if r['mode'] == mode]
            for key, expected in values.items():
                assert statistics.median(r[key] for r in samples) == expected, (raw_path.name, case, mode, key)
    reports.append(report)

for p in [repo/'bench/run_unindexed.py', repo/'bench/run_jsonl_versions.py',
          repo/'bench/run_jsonl_baseline.py', repo/'bench/run_row_cpu.py', work/'unindexed-resource-check.py']:
    ast.parse(read(p), filename=str(p))

assert not dest.exists(), 'refusing to replace archived evidence'
dest.mkdir(parents=True)
write(dest/'.gitignore', '!*.log\n!*.json\n!*.jsonl\n!*.txt\n!*.sh\n!*.py\n')
for pattern in ['unindexed-*', 'vector-scratch-red*', 'build-unindexed-baseline.sh', 'jsonl-unindexed-final*']:
    for p in sorted(work.glob(pattern)):
        if p.is_file() and p.suffix in {'.log', '.json', '.jsonl', '.txt', '.sh', '.py'}:
            shutil.copy2(p, dest/p.name)
        elif p.is_dir() and (p.name.startswith('jsonl-unindexed-final-') or p.name in
                            {'unindexed-final-public', 'unindexed-final-model', 'unindexed-resource', 'unindexed-resource-v2'}):
            for q in p.rglob('*'):
                if q.is_file() and q.suffix in {'.json', '.log', '.txt'}:
                    target = dest/p.name/q.relative_to(p)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(q, target)
shutil.copy2(work/'archive-unindexed.py', dest/'archive-unindexed.py')

dense = med('unindexed-v4-dense')['dense']
varied = med('unindexed-v4-varied')
short = med('unindexed-v4-short')['short_reuse']
overflow = med('unindexed-v4-overflow-repeat')['overflow']
indexed = med('unindexed-v4-indexed')['dense']
timed = [r for r in reports if not r['diagnostic']]
diagnostic = [r for r in reports if r['diagnostic']]
counts = {'timed_samples': sum(r['samples'] for r in timed),
          'timed_queries': sum(r['validated_queries'] for r in timed),
          'diagnostic_samples': sum(r['samples'] for r in diagnostic),
          'diagnostic_queries': sum(r['validated_queries'] for r in diagnostic)}
def f(v): return f'{v:.3f}'
table = '\n'.join(f'| {case} | {f(v["reference"]["query_total_ms"])} | {f(v["scratch"]["query_total_ms"])} | {f(v["reference"]["query_and_mutation_ms"])} | {f(v["scratch"]["query_and_mutation_ms"])} |'
                  for case, v in varied.items())
runtime_table = '\n'.join(f'| {mode} | {f(jsonl["medians"]["uploaded"][mode]["query_ms"])} | {f(jsonl["medians"]["current"][mode]["query_ms"])} | {jsonl["medians"]["uploaded"][mode]["query_ms"]/jsonl["medians"]["current"][mode]["query_ms"]:.3f}× | {jsonl["medians"]["uploaded"][mode]["peak_bytes"]/1048576:.2f} → {jsonl["medians"]["current"][mode]["peak_bytes"]/1048576:.2f} |'
                         for mode in ['unindexed', 'indexed'])
report_text = f'''# 无索引扫描：测量、三轮优化与负面复验

本轮在 `{baseline}` 上开发，日期跨 2026-11-10/11。最终代码、Dockerfile 和学习材料留在第二轮 D 盘工作区，尚未提交、推送或正式提测。本文数据不是正式性能分或 Prompt 分；隐藏用例仍不可见。

## 先拆耗时，再决定方案

基线核心固定 10000 行、128 维、每行 256 字符载荷、Top10、过滤/无过滤各半。诊断模式的 60 查询中位总计 316.443 ms：扫描回调以外约85.309 ms（27%）、全行校验75.982 ms（24%）、Cell物化38.772 ms（12%）、距离33.039 ms（10%）；pread仅0.904 ms。主要问题是重复遍历和对象构造，不能把它描述为物理磁盘读取占主导。

`table_scan` 包含 `scan_consume`，二者都包含内部叶阶段；不可相加。winner refetch在二者外。诊断计时有明显探针开销，只用于定位。生产性能结论均来自关闭计时探针的样本。

## 实施与逐轮自检

1. 无索引精确TopK复用现有单个原始行快照，最多8MiB，和索引路径共享。首次扫描不新分配，第二次完整扫描构建，后续重复使用；点查、提前LIMIT、一般排序仍沿用直接扫描。所有行、包括过滤失败行，都必须保存和完整重验。表ID与保留的index ID=0区分无索引身份；真实索引ID必须为正。
2. 初版主场景764.182→455.031 ms，但每三次读就更新的总周期151.067→161.286 ms，存在真实退化。先移除禁用/拒绝/观察期的逐行append包装，再分离失效与释放：失效清空身份和已用长度，保留同预算arena；第二次扫描仍完整重建。容量变化、超限、扫描异常仍释放。独立开关可恢复释放行为；容量统计诚实报告保留的分配（ExecutionInfo仅TopK采集，其他语句不采集）。
3. 查询内复用一个vector scratch，避免每个候选构造一个向量Cell。复制保留float32位模式/端序，eager路径借用当前行，winner投影仍有自己的值。完整行校验、冷索引一致性检查及距离累加顺序不变。Cell计数真实下降；复制有独立诊断计数，不能假称物化次数相同。

三个开关分别为 `table_scan_snapshot`、`reuse_scan_snapshot_buffer`、`reuse_vector_scratch`，本轮验证后默认开启。单个arena仍有8MiB上限，scratch为一个查询向量的长度；不要求整库驻留。磁盘格式、提交同步和默认JSONL协议未改。

## 正常计时及代价

最终同二进制六轮、三模式轮换全部执行位置，10000×128、200查询/样本、独立2CPU/512MiB容器：参考 `{f(dense['reference']['query_total_ms'])}` ms，仅快照/arena `{f(dense['reuse']['query_total_ms'])}` ms，最终 `{f(dense['scratch']['query_total_ms'])}` ms，最终约 `{dense['reference']['query_total_ms']/dense['scratch']['query_total_ms']:.3f}×`。仅scratch在暖快照上再减少 `{100*(1-dense['scratch']['query_total_ms']/dense['reuse']['query_total_ms']):.2f}%`。有索引同因素三轮 `{f(indexed['indexed_reference']['query_total_ms'])}`→`{f(indexed['indexed']['query_total_ms'])}` ms。

核心基准内存包含生成器与结果，不是正式M。主场景cgroup峰值 `{dense['reference']['local_cgroup_peak_bytes']/1048576:.2f}`→`{dense['scratch']['local_cgroup_peak_bytes']/1048576:.2f}` MiB。首次查询 `{f(dense['reference']['first_query_ms'])}`→`{f(dense['scratch']['first_query_ms'])}` ms；第二次 `{f(dense['reference']['second_query_ms'])}`→`{f(dense['scratch']['second_query_ms'])}` ms，有一次构建成本。第一次无过滤、第二次有过滤，不能直接相减估算构建成本。

变化场景每模式三轮，表中为查询中位ms与包含更新的周期中位ms；周期按每个样本先相加再取中位，不相加独立中位数。

| 场景 | 参考查询 | 最终查询 | 参考完整周期 | 最终完整周期 |
|---|---:|---:|---:|---:|
{table}

稀疏主键的巨大收益来自本实现16层radix遍历的特殊成本，不能推广到所有无索引工作负载。超容量会拒绝缓存并释放，之后继续流式读取。

专项修正：六轮轮换的每三读一更新，释放arena/保留arena完整周期 `{f(short['release']['query_and_mutation_ms'])}`→`{f(short['reuse']['query_and_mutation_ms'])}` ms，minor faults `{short['release']['query_and_mutation_minor_faults']:g}`→`{short['reuse']['query_and_mutation_minor_faults']:g}`；失效后RSS `{short['release']['invalidated_resident_bytes']/1048576:.2f}`→`{short['reuse']['invalidated_resident_bytes']/1048576:.2f}` MiB。保留arena在后续纯写阶段也占内存，只有峰值不能完整体现代价。

长文本超容量三轮出现约4.34%退化，因此追加六轮三模式：参考 `{f(overflow['reference']['query_total_ms'])}`、快照无scratch `{f(overflow['reuse']['query_total_ms'])}`、最终 `{f(overflow['scratch']['query_total_ms'])}` ms。变化小且方向不一致，按接近持平解释；首次失败构建的时间和峰值代价仍存在。原五轮arena实验仅正反序、中间模式位置固定，证据保留但最终结论以轮换复验为准。

## 真实容器与上传版比较

参考镜像 `{jsonl['images']['uploaded']}`，最终镜像 `{images[1]}`。固定10000行128维、每样本1000查询、2CPU/2GiB，三轮交替版本和indexed/unindexed运行顺序，独立生成的同一输入SHA256为 `{jsonl['metadata'][0]['oracle_input_sha256']}`。

| 路径 | 上传版查询ms | 最终查询ms | 时间比 | 容器峰值MiB |
|---|---:|---:|---:|---:|
{runtime_table}

12000次计时查询、12次重新打开全部核对通过，另有暖身和ANN查询。独立答案按返回的sqrt距离及有符号主键排序；不把历史阶段倍率相乘。各模式数据库文件大小不变，写入/索引构建时间和I/O原始字段一并保留，不声称本轮优化了写放大。

## 正确性、资源与审查

- 最终Docker全量38/38通过；公开20/20（95 SQL）；独立模型273检查、150变更、2次强杀恢复；runtime只读根、无网络、持久化及EOF/shutdown回滚通过。
- 无索引快照覆盖两布局、eager/lazy、开关/预算、过滤及零匹配构建、跨表/索引、失败绑定、事务/回滚/重开、winner sink异常、损坏字段、真实late pread错误，以及已实际写入后的UPDATE语句回滚。缩短行集重建不保留旧尾行；分配计数证明arena复用。
- scratch覆盖32种SQL组合，正负零、次正规数、有限极值、非对齐、10种维度、copy/move、宽schema、错误列与边界、预留空间重复复制无新增分配；原有数值极值和等距排序测试继续通过。非小端分支本机未执行。
- 64MiB资源测试先暖小表再扩到20000行、逻辑文本约78.125MiB，检查精确答案和强杀恢复；观测内存/swap详见`unindexed-resource-v2/result.json`。实际JSONL协议不暴露快照计数，保留/超容量状态由专项核心测试证明，运行测试验证相应默认路径的外部行为。WSL不能强制no-swap，因此这不是正式资源准入保证。初次脚本错误地要求Docker接受swap限制，实际配置为-1而观测交换量为0，因此失败；修正为记录真实限制，不修改全局或放宽实际内存/OOM要求，失败目录一并保留。
- 实施前真实RED分别命中未复用快照行、删除后arena释放、候选向量仍物化的断言。最初测试误建两个VECTOR列属于测试夹具错误，修正后才得到有效RED；原错误日志保留。独立审查发现并补上部分UPDATE失败和过滤构建缺口、三模式顺序不平衡及历史rowCPU基准计数假设。历史rowCPU runner现显式关闭后续两项复用以保持单因素对照。

全阶段共{counts['timed_samples']}个正常计时样本、{counts['timed_queries']}查询，另{counts['diagnostic_samples']}个诊断样本、{counts['diagnostic_queries']}查询；包含初版与负面实验，不将其冒充最终版本重复数。最终正常核心实验120样本8688查询；真实JSONL对照另计。

本轮停止继续扩大生产改动：残余诊断中完整校验约72ms、距离约32ms（60查询，含探针），继续压缩需更细的热点证据；不跳过校验、不改变精确数值、不引入未经验证的近似剪枝或同步变化。后续可研究扫描/回调固定开销与更新的16层路径复制，但需独立实验。

## 学习材料与可复核入口

[原始数据与摘要](../bench/results/unindexed-20261111/manifest.json)、[执行计划](superpowers/plans/2026-11-10-unindexed-experiment-plan.md)、[真实学习记录](../agent/conversation.json)。两个conversation.json副本相同、小于128KiB，包含真实用户授权、Agent推理归属、失败及修正证据。平台0–100有效性及材料摘要规则按平台判定；不生成自评分、不截断或换算本地分数，也不臆造内部评价字段。
'''
write(repo/'docs/unindexed-results.md', report_text)

learning = load(repo/'agent/conversation.json')
assert not any(e['id'] == 'e57' for e in learning['experiments'])
for ident, content in [
    ('u31', 'Prompt结果无效、待复核、超出平台0–100范围或缺少材料摘要，本项不参与排名；未截断或换算本地分数。注意这个'),
    ('u32', '补充的规则说明'),
    ('u33', '无索引是我们的弱点部分是吗'),
    ('u34', '可以，按你的方法进实验'),
    ('u35', '不断优化，直至到你觉得ok了为止，优化完一轮之后就自觉检查自己的薄弱点，然后针对薄弱点继续优化')]:
    learning['conversations'].append({'id': ident, 'role': 'user', 'content': content})
events = [
    ('e57', 'tool', '先拆分无索引成本', '60查询诊断中位316.443ms，回调外85.309、全行校验75.982、物化38.772、距离33.039、pread0.904；包含阶段不相加。正常计时另跑。'),
    ('e58', 'tool', '发现第一轮快照的负面结果', '主场景764.182→455.031ms，但每三读一更新完整周期151.067→161.286ms。最初两个VECTOR的测试夹具错误已修正；随后baseline RED命中预期快照行计数。禁用/拒绝路径取消逐行append包装。'),
    ('e59', 'tool', '单arena复用与实验顺序纠错', f'删除后保留容量用例在旧实现真实RED；失效只保留分配，所有内容重新完整校验。六轮轮换短读写周期release {f(short["release"]["query_and_mutation_ms"])}→reuse {f(short["reuse"]["query_and_mutation_ms"])}ms；保留RSS代价。初五轮正反序未平衡中间模式，独立审查指出后重跑，旧数据保留。'),
    ('e60', 'tool', '查询内向量临时空间复用', f'旧路径候选向量Cell计数非零而新要求零，真实RED后实现。32种SQL组合、位模式和零新增分配测试通过；完整行校验及距离累加不变。最终六轮核心参考{f(dense["reference"]["query_total_ms"])}→{f(dense["scratch"]["query_total_ms"])}ms；单独scratch从{f(dense["reuse"]["query_total_ms"])}ms继续下降。'),
    ('e61', 'tool', '最终容器、实际协议与负面复验', f'38/38内部、20/20公开95SQL、273模型/2kill、runtime通过；12000真实JSONL查询12重启通过。无索引{f(jsonl["medians"]["uploaded"]["unindexed"]["query_ms"])}→{f(jsonl["medians"]["current"]["unindexed"]["query_ms"])}ms；有索引{f(jsonl["medians"]["uploaded"]["indexed"]["query_ms"])}→{f(jsonl["medians"]["current"]["indexed"]["query_ms"])}ms。超容量初3轮约+4.34%，追加6轮238.785→233.422ms按接近持平解释，不隐藏负面记录。'),
    ('e62', 'tool', '小表预热后大表写入资源与崩溃检查', '64MiB容器先预热1000行快照，再扩至20000行约78.125MiB逻辑文本，超容量回退和未提交DELETE强杀恢复通过；保留当前/峰值/交换观测，WSL不能保证no-swap，不宣称正式准入。')]
for ident, source, purpose, output in events:
    learning['experiments'].append({'id': ident, 'source': source, 'purpose': purpose, 'output': output,
                                   'evidence': 'bench/results/unindexed-20261111/manifest.json'})
learning['version_context'].update(latest_uploaded_base=baseline,
    current_changes='Local unindexed snapshot, arena reuse and vector scratch optimization on latest uploaded base; full local validation. Not committed, pushed or formally submitted in this round.')
learning['final_validation'] = {'source': 'tool', 'internal_log_tail': '\n'.join(internal.strip().splitlines()[-5:]),
    'runtime_output': runtime_output, 'image_ids': '\n'.join(images),
    'public': '20/20,95 SQL', 'model': model, 'runtime_queries':12000,'runtime_restarts':12,
    'state': 'Verified local working tree; no new commit/push/formal submission. Historical validations remain in prior experiments.'}
learning['performance_caveats']['unindexed'] = '本轮无索引快照增加最多8MiB arena；失效可保留驻留内存，第二次扫描构建有成本；超容量与交错读写负面结果完整保留。所有分数仍由平台给出。'
learning['limitations'].append('用户确认Prompt有效性文字是规则提醒，不是某次提交失败。材料摘要字段含义未由平台明确；不伪造内部评价字段，不自评分或换算。')
ui = next(i for i,e in enumerate(learning['conversations']) if e['id']=='u35')
ei = next(i for i,e in enumerate(learning['experiments']) if e['id']=='e58')
learning['review_navigation']['items'].append({'chain':'测量无索引弱点并逐轮修正退化',
    'student_evidence':{'pointer':f'/conversations/{ui}/content','quote':learning['conversations'][ui]['content']},
    'result_evidence':[{'pointer':f'/experiments/{ei}/output','quote':learning['experiments'][ei]['output']}],
    'attribution_limit':'用户指定持续自检目标；具体假设、实现、测试及审查修正来自Agent/工具，不视作学生独立技术解释，也不把一次迭代拆成多次学习得分。'})
learning_data = (json.dumps(learning, ensure_ascii=False, indent=2)+'\n').encode('utf-8')
assert len(learning_data) < 128*1024
for p in [repo/'agent/conversation.json', repo/'conversation.json']: p.write_bytes(learning_data)
assert (repo/'agent/conversation.json').read_bytes() == (repo/'conversation.json').read_bytes()

for file, line in [(repo/'README.md', '\n本轮新增[无索引快照、缓冲复用与临时向量优化的完整实验](docs/unindexed-results.md)，保留三轮迭代、负面结果、38项回归与真实JSONL对照。\n'),
                   (repo/'docs/admission-validation.md', '\n## 2026-11-11 最新本地优化复验\n\n详见[无索引优化验证](unindexed-results.md)：38/38内部、20/20公开（95SQL）、273模型检查/2强杀、runtime与12000实际JSONL查询/12重启通过。全部叠加在64f43d1上传基线上；没有新推送或正式提测，不能将本地通过称为隐藏准入保证。\n'),
                   (repo/'bench/README.md', '\n## 无索引与查询临时空间实验\n\n`run_unindexed.py` 支持参考、快照、arena复用、scratch及indexed对照，三模式轮换全部执行位置，独立精确答案和重开全行校验。源码、原始输出、一次构建成本、RSS代价和负面结果见[完整报告](../docs/unindexed-results.md)。历史`run_row_cpu.py`显式关闭新增复用，使旧CPU实验仍只改变原有两个因素。\n')]:
    write(file, read(file)+line)
plan = repo/'docs/superpowers/plans/2026-11-10-unindexed-experiment-plan.md'
write(plan, read(plan).replace('- [ ]','- [x]'))

sources = [repo/'Dockerfile', repo/'CMakeLists.txt']
for folder in ['src','include','tests','bench']:
    sources.extend(p for p in (repo/folder).rglob('*') if p.is_file() and
                   'results' not in p.relative_to(repo).parts and p.suffix in {'.cpp','.hpp','.py'})
manifest = {'base_commit':baseline,'test_image':images[0],'runtime_image':images[1],
    'scope':'Local experiment and validation, not official score or hidden admission',
    'counts':counts,'learning_bytes':len(learning_data), 'learning_sha256':sha(repo/'agent/conversation.json'),
    'source_sha256':{str(p.relative_to(repo)):sha(p) for p in sorted(set(sources))},
    'files':{str(p.relative_to(dest)):sha(p) for p in sorted(dest.rglob('*')) if p.is_file()}}
dump(dest/'manifest.json', manifest)
checkpoint = load(root/'outputs/checkpoint.json')
checkpoint.update(date='2026-11-11', worktree_clean=False,pushed=False,
    last_pushed_commit=baseline, commit=baseline, runtime_image=images[1],test_image=images[0],
    current_milestone='Three rounds of unindexed optimization and independent full validation complete locally; uncommitted and unpushed.',
    final_container_tests='38/38',learning_bytes=len(learning_data),material_sha256=sha(repo/'agent/conversation.json'),
    report='repo/docs/unindexed-results.md', next_steps=['Review completed local optimization; commit/push only when user requests.'])
checkpoint['unindexed_optimization']={'committed':False,'pushed':False,'base_commit':baseline,
    'core_dense':dense,'jsonl':jsonl['medians'],'counts':counts,
    'resource_check':resource,'manifest_sha256':sha(dest/'manifest.json')}
dump(root/'outputs/checkpoint.json',checkpoint)
print(json.dumps({'archived_files':len(manifest['files']),'counts':counts,
                  'learning_bytes':len(learning_data),'learning_sha256':manifest['learning_sha256'],
                  'runtime':images[1],'jsonl':jsonl['medians']},ensure_ascii=False),flush=True)
