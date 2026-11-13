"""64 MiB default-JSONL check; run only in the verified D-backed MiniVectorDB WSL.

Usage: python3 work/score-first-resource-check.py IMAGE
Uses the official Session transport; does not override the image entrypoint.
Snapshot states are inferred from the default implementation and dataset sizes,
not observed through JSONL (which does not expose internal snapshot counters).
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


LIMIT = 64 * 1024 * 1024
SNAPSHOT_BUDGET = 8 * 1024 * 1024
INITIAL_ROWS = 1000
FINAL_ROWS = 20000
BODY = 'x' * 4096
BATCH = 100
TOPK_SQL = "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10"
EXPECTED_ENTRYPOINT = ['/usr/local/bin/minivectordb', '--protocol', 'jsonl', '--data-dir', '/data']


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def expected_topk(count):
    ranks = sorted((math.sqrt((i % 97) ** 2 + (i % 13) ** 2), i) for i in range(count))
    return [[key, distance] for distance, key in ranks[:10]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='Required final runtime image or immutable image ID')
    args = parser.parse_args()
    work = Path(__file__).resolve().parent
    require(work == Path('/mnt/d/Codex/MiniVectorDB/work'), 'Run the D: copy inside the dedicated WSL distribution')
    data_root = Path('/var/lib/minivectordb-unindexed-resource')
    require(data_root.resolve() == data_root, 'Data root must not redirect through a symlink')
    out = work / 'score-first-resource'
    require(not out.exists() and not out.is_symlink(), 'Refusing to overwrite resource results')
    os.environ['DOCKER_CONFIG'] = '/var/lib/minivectordb-docker-client'
    require(Path(os.environ['DOCKER_CONFIG']).is_dir(), 'Dedicated Docker configuration is missing')
    data_root.mkdir(exist_ok=True)
    # Session owns and cleans both of its TemporaryDirectories. No broad deletion
    # of the data root or another session's files is performed by this script.
    tempfile.tempdir = str(data_root)
    fs_type = subprocess.check_output(['stat', '-f', '-c', '%T', str(data_root)], text=True, timeout=30).strip()
    require(fs_type in {'ext2/ext3', 'ext4'}, 'Database files must use the dedicated WSL Linux filesystem')
    protocol = work / 'official-protocol-f4d4e26' / 'protocol_check'
    config_path = protocol / 'config.json'
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(protocol))
    from _transport import Session
    cfg = json.loads(config_path.read_text(encoding='utf-8'))
    cfg.update(memory='64m', case_timeout=1800, request_timeout=120)
    inspected = subprocess.run(['docker', 'image', 'inspect', args.image], check=True,
                               capture_output=True, text=True, timeout=30)
    image_info = json.loads(inspected.stdout)[0]
    require(image_info['Config'].get('Entrypoint') == EXPECTED_ENTRYPOINT,
            'The supplied image is not the expected final JSONL runtime')
    require(not image_info['Config'].get('Cmd'), 'Unexpected extra runtime command arguments')
    image_id = image_info['Id']
    require(image_id.startswith('sha256:'), 'Could not pin the runtime image ID')
    out.mkdir(exist_ok=False)
    metadata = {
        'requested_image': args.image, 'image_id': image_id,
        'entrypoint': image_info['Config']['Entrypoint'], 'config': cfg,
        'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'transport_sha256': hashlib.sha256((protocol / '_transport.py').read_bytes()).hexdigest(),
        'official_config_sha256': hashlib.sha256(config_path.read_bytes()).hexdigest(),
        'data_root': str(data_root), 'data_filesystem': fs_type, 'output': str(out),
        'initial_rows': INITIAL_ROWS, 'final_rows': FINAL_ROWS, 'body_bytes_per_row': len(BODY),
        'requested_memory_swap_limit_bytes': LIMIT,
        'swap_policy': 'Session requests memory-swap equal to memory. This WSL host may ignore or normalize '
                       'the request; observed Docker and cgroup limits are reported without a no-swap guarantee.',
        'index_created': False, 'snapshot_budget_assumed_from_default': SNAPSHOT_BUDGET,
        'warm_snapshot_bytes_estimate': INITIAL_ROWS * (2 * 4 + 8) + 8,
        'large_numeric_payload_estimate': FINAL_ROWS * (2 * 4 + 8) + 8,
        'cache_scope': 'Numeric payload fits the typed budget; this tests dataset larger than RAM, not typed-budget refusal.',
        'large_text_bytes': FINAL_ROWS * len(BODY),
        'scope': '64 MiB runtime container; host generates input and checks answers. '
                 'D-backed WSL backing is verified by the caller. Swap settings and observed usage are reported; '
                 'this is not official resource admission or a direct measurement of internal cache state.'}
    require(metadata['warm_snapshot_bytes_estimate'] < SNAPSHOT_BUDGET, 'Warm fixture would not fit')
    require(metadata['large_text_bytes'] > LIMIT, 'Final fixture must exceed the memory limit')
    save(out / 'metadata.json', metadata)
    session = Session(cfg, out / 'stderr.log', image=image_id, data_root=str(data_root))
    checkpoints = []
    validations = []
    generation = 0
    started = time.monotonic()

    def q(sql):
        response = session.request(sql, mode='exact')
        require(response['ok'] is True, f'SQL request failed: {response!r}')
        return response

    def sample(phase):
        inspection = json.loads(session.docker('inspect', session.name).stdout)[0]
        state = inspection['State']
        require(state['Running'] and state['Pid'] > 0, 'Runtime is not running')
        lines = Path(f"/proc/{state['Pid']}/cgroup").read_text().splitlines()
        relative = next(line.split(':', 2)[2] for line in lines if line.startswith('0::'))
        cgroup_root = Path('/sys/fs/cgroup').resolve()
        cgroup = (cgroup_root / relative.lstrip('/')).resolve()
        require(cgroup.is_relative_to(cgroup_root), 'Unexpected cgroup path')
        events = {key: int(value) for key, value in
                  (line.split() for line in (cgroup / 'memory.events').read_text().splitlines())}
        files = [path for path in Path(session.bind_data.name).rglob('*')
                 if not path.is_symlink() and path.is_file()]
        value = {
            'phase': phase, 'generation': generation, 'elapsed_seconds': time.monotonic() - started,
            'memory_limit': inspection['HostConfig']['Memory'],
            'configured_memory_swap_limit': inspection['HostConfig']['MemorySwap'],
            'cgroup_memory_limit': (cgroup / 'memory.max').read_text().strip(),
            'memory_current': int((cgroup / 'memory.current').read_text()),
            'memory_peak': int((cgroup / 'memory.peak').read_text()),
            'swap_current': int((cgroup / 'memory.swap.current').read_text()),
            'swap_max': (cgroup / 'memory.swap.max').read_text().strip(),
            'memory_events': events, 'oom_killed': state['OOMKilled'],
            'file_bytes': sum(path.stat().st_size for path in files), 'file_count': len(files)}
        checkpoints.append(value)
        save(out / 'checkpoints.json', checkpoints)
        require(value['memory_limit'] == LIMIT and value['cgroup_memory_limit'] == str(LIMIT),
                'The actual container memory limit is not 64 MiB')
        require(not value['oom_killed'] and events.get('oom', 0) == 0 and events.get('oom_kill', 0) == 0,
                'An OOM event occurred')
        return value

    def insert(begin, end):
        require(begin % BATCH == 0 and end % BATCH == 0, 'Unexpected batch boundary')
        for first in range(begin, end, BATCH):
            values = ','.join(f"({i},'[{i % 97},{i % 13}]','{BODY}')" for i in range(first, first + BATCH))
            require(q('INSERT INTO t VALUES ' + values)['affected_rows'] == BATCH, 'Insert count mismatch')

    def topk(count, repetitions, phase):
        expected = expected_topk(count)
        for attempt in range(repetitions):
            response = q(TOPK_SQL)
            require(response['rows'] == expected, f'{phase} TopK mismatch at attempt {attempt + 1}')
            validations.append({'phase': phase, 'attempt': attempt + 1, 'topk': response['rows']})

    def all_ids(count, phase):
        rows = q('SELECT id FROM t')['rows']
        require(all(isinstance(row, list) and len(row) == 1 and type(row[0]) is int for row in rows),
                f'{phase} malformed ID response')
        ids = sorted(row[0] for row in rows)
        require(ids == list(range(count)), f'{phase} complete ID set differs (including duplicates)')
        validations.append({'phase': phase, 'verified_ids': count,
                            'ids_sha256': hashlib.sha256(json.dumps(ids).encode()).hexdigest()})
        save(out / 'validations.json', validations)

    try:
        session.start()
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),body VARCHAR(4096))')
        insert(0, INITIAL_ROWS)
        sample('before_warm')
        topk(INITIAL_ROWS, 3, 'warm')
        sample('after_warm')
        all_ids(INITIAL_ROWS, 'warm')
        # No SELECT between these inserts: retain the warmed arena while every
        # committed INSERT invalidates its old row set, then grow stored text past RAM.
        insert(INITIAL_ROWS, FINAL_ROWS)
        appended = sample('after_append')
        require(appended['file_bytes'] > LIMIT, 'Persisted data did not exceed the container memory limit')
        topk(FINAL_ROWS, 4, 'large_data')
        sample('after_query')
        all_ids(FINAL_ROWS, 'large_data')
        q('BEGIN')
        require(q('DELETE FROM t WHERE id=0')['affected_rows'] == 1, 'Uncommitted delete count mismatch')
        require(q('SELECT id FROM t WHERE id=0')['rows'] == [], 'Uncommitted delete not visible')
        sample('before_crash')
        session.crash()
        generation += 1
        session.start()
        sample('after_restart')
        require(q('SELECT body FROM t WHERE id=0')['rows'] == [[BODY]], 'Deleted row was not restored after crash')
        require(q(f'SELECT body FROM t WHERE id={FINAL_ROWS - 1}')['rows'] == [[BODY]], 'Last committed row was lost')
        topk(FINAL_ROWS, 4, 'recovery')
        all_ids(FINAL_ROWS, 'recovery')
        sample('after_recovery')
        session.shutdown()
        result = {'passed': True, 'metadata': metadata, 'checkpoints': checkpoints, 'validations': validations}
        save(out / 'result.json', result)
        print(json.dumps(result), flush=True)
    except BaseException as error:
        failure = {'passed': False, 'error': repr(error), 'checkpoints': checkpoints, 'validations': validations}
        try:
            inspection = session.docker('inspect', session.name, check=False)
            if inspection.returncode == 0:
                failure['container_state'] = json.loads(inspection.stdout)[0]['State']
        except Exception as diagnostic_error:
            failure['diagnostic_error'] = repr(diagnostic_error)
        save(out / 'result.json', failure)
        raise
    finally:
        session.close()


if __name__ == '__main__':
    main()
