"""Archive only this round's evidence under the D: competition checkout."""
import hashlib, json, shutil, statistics
from pathlib import Path

root = Path('D:/Codex/MiniVectorDB')
repo, work = root/'repo', root/'work'
dest = repo/'bench/results/score-first-20261111'
dest.mkdir(exist_ok=True)
def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(str(source), str(target))
def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)+'\n', encoding='utf-8')

for pattern in ['typed-first-*', 'typed-validated-*', 'typed-red.*', 'commit-policy-red.*',
                'validated-vector-red.*', 'score-first-*', 'score-policy-*',
                'score-combined*', 'score-memory*', 'score-kernel*',
                'jsonl-score-first-final-*', 'typed-controlled.sh',
                'typed-phase-a.cpp', 'run_typed-phase-a.py']:
    for source in work.glob(pattern):
        if source.is_file() and source.suffix in ['.json', '.jsonl', '.log', '.sh', '.py', '.txt', '.cpp']:
            copy_file(source, dest/source.name)
        elif source.is_dir():
            for item in source.rglob('*'):
                if item.is_file() and item.suffix in ['.json', '.log', '.txt']:
                    copy_file(item, dest/source.name/item.relative_to(source))
for name in ['2026-11-11-score-rules-review.md', '2026-11-11-score-material-audit.json',
             '2026-11-11-material-validator-probes.json', '2026-11-11-commit-policy-review.md',
             '2026-11-11-final-candidate-review.md', '2026-11-11-jsonl-tbm-review.md']:
    copy_file(root/'outputs'/name, dest/name)
copy_file(root/'build/score-first-core/Testing/Temporary/LastTest.log', dest/'core-kernel-pre-default-LastTest.log')
copy_file(Path(__file__), dest/'archive-score-first.py')

comparison = json.loads((work/'jsonl-score-first-final-summary.json').read_text(encoding='utf-8'))
assert comparison['queries_verified'] == 12000 and comparison['restarts'] == 12
def io(resource):
    assert isinstance(resource.get('io.stat'), str), 'Missing block I/O observation'
    device = resource['data_device']
    by_device = {}
    for line in resource['io.stat'].splitlines():
        fields = line.split()
        by_device[fields[0]] = dict((k, int(v)) for k,v in (field.split('=') for field in fields[1:]))
    values = by_device.get(device, {'rbytes':0,'wbytes':0})
    return values['rbytes'] + values['wbytes'], by_device
def memory_stat(resource, key):
    values = dict(line.split() for line in resource['memory.stat'].splitlines())
    return int(values[key])
raw = []
for sample in comparison['samples']:
    ready, write, before, query = [sample[k] for k in ['resources_ready','resources_after_write','resources_before_queries','resources_after_queries']]
    assert len({r['data_device'] for r in [ready,write,before,query]}) == 1
    wb, wd = io(write); qb, qd = io(query)
    row = {'version':sample['version'], 'indexed':sample['indexed'], 'round':sample['comparison_round'],
           'data_device':query['data_device'], 'write_cumulative_io_bytes':wb,
           'query_cumulative_io_bytes':qb, 'write_interval_io_bytes':wb-io(ready)[0],
           'query_interval_io_bytes':qb-io(before)[0],
           'write_peak_bytes':int(write['memory.peak']), 'query_peak_bytes':int(query['memory.peak']),
           'write_ms':sample['write_ms'], 'query_ms':sample['query_ms'],
           'index_build_ms':sample['index_build_ms'] or 0,
           'request_cycle_ms':sample['write_ms']+(sample['index_build_ms'] or 0)+sample['query_ms'],
           'file_bytes':sum(sample['files_final'].values()),
           'write_devices':wd, 'query_devices':qd,
           'write_dirty_bytes':memory_stat(write,'file_dirty'), 'write_writeback_bytes':memory_stat(write,'file_writeback'),
           'query_dirty_bytes':memory_stat(query,'file_dirty'), 'query_writeback_bytes':memory_stat(query,'file_writeback'),
           'swap_current_bytes':int(query['memory.swap.current'])}
    raw.append(row)
fields=['write_ms','query_ms','index_build_ms','request_cycle_ms','write_cumulative_io_bytes',
        'query_cumulative_io_bytes','write_interval_io_bytes','query_interval_io_bytes',
        'write_peak_bytes','query_peak_bytes','file_bytes','write_dirty_bytes',
        'write_writeback_bytes','query_dirty_bytes','query_writeback_bytes','swap_current_bytes']
medians={}
for version in ['uploaded','current']:
    medians[version]={}
    for indexed in [False,True]:
        group=[r for r in raw if r['version']==version and r['indexed']==indexed]
        assert len(group)==3
        medians[version]['indexed' if indexed else 'unindexed']={k:statistics.median(r[k] for r in group) for k in fields}
report={'medians':medians, 'samples':raw, 'devices':sorted({r['data_device'] for r in raw}),
        'units':'milliseconds and bytes; three-round medians, fresh container per sample',
        'T_scope':'Sum of measured requests including JSONL transport; excludes warmup and inter-request oracle validation.',
        'B_scope':'Cgroup rbytes+wbytes on verified data_device up to sample, includes startup/load; not database file length; not official B or shutdown/drained writeback.',
        'M_scope':'Container cumulative memory.peak since start at each phase; query peak includes load/index/warmup, not an isolated query peak.',
        'score':None, 'formal_result':'Only baseline524133 has user-reported official admission/E; this candidate not pushed/submitted.'}
save(dest/'tbm-summary.json', report)
print(json.dumps({'medians':medians,'devices':report['devices']}, ensure_ascii=False))
