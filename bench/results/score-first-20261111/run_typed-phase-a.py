"""Controlled index-presence and table-snapshot experiments, not official scores."""
from pathlib import Path
import argparse
import hashlib
import json
import statistics
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--output', required=True)
p.add_argument('--binary', required=True)
p.add_argument('--rounds', type=int, default=3)
p.add_argument('--suite', choices=['dense', 'varied'], default='dense')
p.add_argument('--case')
p.add_argument('--modes', default='reference,typed')
p.add_argument('--profile', action='store_true')
a = p.parse_args()
assert a.rounds > 0
root = Path('/mnt/d/Codex/MiniVectorDB')
out = Path(a.output).resolve()
binary = Path(a.binary).resolve()
assert out.is_relative_to(root) and not out.exists()
assert binary.is_relative_to(root) and binary.is_file()
out.parent.mkdir(parents=True, exist_ok=True)
source_hash = hashlib.sha256((root/'repo/bench/typed.cpp').read_bytes()).hexdigest()
binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
image = 'sha256:e1cdb1e7239b915403b846f0c78ab19a4d032e58ffc77c6aba6d05552a38bc07'
docker = ['docker', '--config', '/var/lib/minivectordb-docker-client']
data_root = Path('/var/lib/minivectordb-typed-bench')
data_root.mkdir(exist_ok=True)
settings = {'reference': {'MVDB_NO_INDEX':'1','MVDB_TYPED_CACHE':'0'},
            'typed': {'MVDB_NO_INDEX':'1','MVDB_TYPED_CACHE':'1'},
            'indexed_reference': {'MVDB_TYPED_CACHE':'0'},
            'indexed': {'MVDB_TYPED_CACHE':'1'}}
modes = a.modes.split(',')
assert len(modes) >= 2 and len(set(modes)) == len(modes) and set(modes) <= set(settings)
cases = {'dense': (10000, 60 if a.profile else 200, {})}
if a.suite == 'varied':
    cases = {'wide': (2000, 40, {'MVDB_DIMENSIONS': '768'}),
             'overflow': (2000, 40, {'MVDB_PAYLOAD': '5000'}),
             'interleaved': (2000, 40, {'MVDB_INVALIDATE_EVERY': '1'}),
             'short_reuse': (2000, 42, {'MVDB_INVALIDATE_EVERY': '3'}),
             'large': (30000, 40, {}),
             'sparse': (2000, 40, {'MVDB_SPARSE_KEYS': '1'}),
             'unicode': (2000, 40, {'MVDB_UNICODE_BODY': '1'}),
             'small': (2000, 40, {'MVDB_DIMENSIONS': '16'}),
             'wide_schema': (2000, 40, {'MVDB_EXTRA_COLUMNS': '60'}),
             'disabled': (2000, 40, {'MVDB_SNAPSHOT_BYTES': '0'})}
if a.case:
    cases = {a.case: cases[a.case]}
records = []
for case, (rows, queries, extra) in cases.items():
    for round_no in range(a.rounds):
        # Rotate every mode through every execution position, reversing each block.
        # Simple reversal leaves the middle of a three-mode comparison fixed.
        base = modes if (round_no // len(modes)) % 2 == 0 else list(reversed(modes))
        offset = round_no % len(modes)
        order = base[offset:] + base[:offset]
        if len(modes) == 2:
            order = modes if round_no % 2 == 0 else list(reversed(modes))
        for mode in order:
            print('START', case, round_no + 1, mode, 'profile' if a.profile else 'timed', flush=True)
            with tempfile.TemporaryDirectory(prefix='owned-', dir=data_root) as directory:
                env = {'MVDB_DATA_CACHE_BYTES': '16777216', 'MVDB_SCAN_CACHE_BYTES': '0',
                       'MVDB_PACKED': '1', 'MVDB_BENCH_CGROUP': '1', 'MVDB_PAIRED': '1',
                       'MVDB_SNAPSHOT_BYTES': '8388608', **extra, **settings[mode]}
                if a.profile:
                    env['MVDB_PROFILE'] = '1'
                cmd = docker + ['create', '--read-only', '--network', 'none', '--cpus', '2', '--memory', '512m',
                                '--mount', f'type=bind,src={directory},dst=/data',
                                '--mount', f'type=bind,src={binary},dst=/bench/run,readonly', '--entrypoint', '/bench/run']
                for key, value in env.items():
                    cmd += ['--env', key + '=' + value]
                cmd += [image, '/data/database.mvdb', str(rows), str(queries), '1', '1']
                container = subprocess.check_output(cmd, text=True, timeout=60).strip()
                try:
                    run = subprocess.run(docker + ['start', '--attach', container], text=True, capture_output=True, timeout=900)
                    if run.returncode:
                        raise RuntimeError(run.stdout + run.stderr)
                    row = json.loads(run.stdout)
                finally:
                    subprocess.run(docker + ['rm', '--force', container], check=True, capture_output=True, timeout=30)
                assert row['index_present'] == (mode in ['indexed', 'indexed_reference'])
                assert row['validated_queries'] == queries and row['first_query_validated']
                assert row['recovered_rows'] == rows and row['profile_enabled'] == a.profile
                assert row['typed_enabled'] == (settings[mode]['MVDB_TYPED_CACHE'] == '1')
                assert row['reuse_vector_scratch'] == (settings[mode].get('MVDB_VECTOR_SCRATCH', '1') == '1')
                row['query_and_mutation_ms'] = row['query_total_ms'] + row['churn_ms']
                row.update(case=case, mode=mode, round=round_no + 1, binary_sha256=binary_hash,
                           benchmark_source_sha256=source_hash, base_commit='524133baf0a4ed947805236c02cce9cecffc0be3',
                           scope='Core benchmark includes driver memory; profile timings are diagnostic, not official score.')
                records.append(row)
                with out.open('a') as f:
                    f.write(json.dumps(row) + '\n')
                print('PASS query_ms', row['query_total_ms'], 'snapshot_rows', row['snapshot_rows'], flush=True)
summary = {}
for case in cases:
    group = [r for r in records if r['case'] == case]
    for key in ['result_digest', 'rows_examined', 'distances_computed', 'recovered_rows']:
        assert len({r[key] for r in group}) == 1, (case, key)
    if not (set(modes) & {'indexed', 'indexed_reference'}):
        for key in ['database_bytes']:
            assert len({r[key] for r in group}) == 1, (case, key)
    # Scratch is not a decoded Cell. Compare counters only within the same path,
    # and assert candidate vectors are not materialized on lazy unindexed TopK.
    for r in group:
        if r['reuse_vector_scratch'] and not r['index_present']:
            assert r['vector_values_materialized'] == 0
    summary[case] = {}
    for mode in modes:
        subset = [r for r in group if r['mode'] == mode]
        summary[case][mode] = {key: statistics.median(r[key] for r in subset) for key in [
            'write_ms', 'index_build_ms', 'query_total_ms', 'query_unfiltered_ms', 'query_filtered_ms',
            'first_query_ms', 'second_query_ms', 'snapshot_capacity', 'snapshot_rows', 'database_bytes', 'churn_ms',
            'query_and_mutation_ms', 'typed_rows', 'typed_capacity',
            'warm_resident_bytes', 'invalidated_resident_bytes', 'query_and_mutation_minor_faults',
            'process_maxrss_kib', 'local_cgroup_peak_bytes']}
assert hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash
assert hashlib.sha256((root/'repo/bench/typed.cpp').read_bytes()).hexdigest() == source_hash
report = {'medians': summary, 'samples': len(records), 'validated_queries': sum(r['validated_queries'] for r in records),
          'reopen_checks': len(records), 'diagnostic': a.profile, 'no_official_score': True,
          'order': 'rotating positions, reverse each full block',
          'profile_note': 'table_scan and scan_consume are inclusive. Difference estimates traversal outside callbacks; do not sum enclosing and leaf timers.'}
out.with_suffix('.summary.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
