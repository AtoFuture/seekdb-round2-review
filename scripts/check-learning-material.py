"""Read-only checks for the two learning files and their local evidence binding.

This checks packaging and provenance, not the competition's Prompt score.
"""
from pathlib import Path
import hashlib
import json
import math
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HISTORICAL_SOURCE_COMMIT = '524133baf0a4ed947805236c02cce9cecffc0be3'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pairs_unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def is_within(path, directory):
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def path_in_repo(name):
    path = (ROOT / name).resolve()
    require(is_within(path, ROOT), 'Reference leaves repository: ' + name)
    require(path.is_file(), 'Missing file: ' + name)
    return path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invalid_constant(value):
    raise ValueError('Non-standard JSON constant: ' + value)


def finite_float(value):
    result = float(value)
    require(math.isfinite(result), 'Non-finite JSON number')
    return result


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs_unique,
                      parse_constant=invalid_constant, parse_float=finite_float)


def preserve_prefix(current, previous, label):
    for field in ['conversations', 'experiments', 'agent_observations']:
        old = previous[field]
        require(isinstance(old, list) and isinstance(current[field], list),
                'Expected record arrays: ' + field)
        require(current[field][:len(old)] == old, label + ' changed historical ' + field)


def verify_selection(current, archive):
    """A shorter submission may select records, but may not rewrite their source."""
    for field in ['conversations', 'experiments', 'agent_observations']:
        require(isinstance(current[field], list) and isinstance(archive[field], list),
                'Expected record arrays: ' + field)
        original = {row['id']: row for row in archive[field]}
        require(len(original) == len(archive[field]), 'Duplicate archived record IDs: ' + field)
        seen = set()
        for row in current[field]:
            identity = row['id']
            require(identity not in seen, 'Duplicate selected record ID: ' + identity)
            seen.add(identity)
            require(identity in original and row == original[identity],
                    'Selected record changed or invented: ' + identity)
    require(current['conversations'] == archive['conversations'],
            'Curated material must preserve every recorded user statement')


def resolve_pointer(document, pointer):
    require(isinstance(pointer, str) and pointer.startswith('/'), 'Invalid JSON pointer')
    target, parent = document, None
    for raw in pointer[1:].split('/'):
        require(re.search(r'~(?![01])', raw) is None, 'Invalid JSON pointer escape')
        part = raw.replace('~1', '/').replace('~0', '~')
        parent = target
        if isinstance(target, list):
            require(re.fullmatch(r'0|[1-9][0-9]*', part) is not None, 'Invalid array index')
            index = int(part)
            require(index < len(target), 'JSON pointer index out of range')
            target = target[index]
        else:
            require(isinstance(target, dict) and part in target, 'Unresolved JSON pointer: ' + pointer)
            target = target[part]
    require(isinstance(target, str), 'Evidence pointer must resolve to a string: ' + pointer)
    return target, parent


def check_references(document):
    count = 0
    def visit(value):
        nonlocal count
        if isinstance(value, dict):
            if 'pointer' in value:
                target, parent = resolve_pointer(document, value['pointer'])
                if 'id' in value:
                    require(isinstance(parent, dict) and parent.get('id') == value['id'],
                            'Record ID/pointer mismatch: ' + value['pointer'])
                if 'quote' in value:
                    quote = value['quote']
                    require(isinstance(quote, str) and 0 < len(quote) <= 240 and quote in target,
                            'Invalid or mismatched quotation: ' + value['pointer'])
                count += 1
            for member in value.values():
                visit(member)
        elif isinstance(value, list):
            for member in value:
                visit(member)
    visit(document)
    return count


def git_file(commit, name):
    require(isinstance(commit, str) and re.fullmatch(r'[0-9a-f]{40}', commit) is not None,
            'Historical source requires a full immutable commit SHA')
    require(isinstance(name, str) and name and not name.startswith('/') and
            '\\' not in name and ':' not in name and '..' not in name.split('/'),
            'Invalid historical Git path')
    result = subprocess.run(['git', '-c', 'core.bare=false', 'show', commit + ':' + name],
                            cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30)
    require(result.returncode == 0, 'Historical Git object unavailable: ' + name)
    return result.stdout


def main():
    binding = read_json(path_in_repo('agent/material-digest.json'))
    require(binding['kind'] == 'local-material-integrity-manifest', 'Wrong integrity manifest')
    files = binding['material_files']
    require(set(files) == {'conversation.json', 'agent/conversation.json'}, 'Wrong material files')
    material_bytes = path_in_repo('conversation.json').read_bytes()
    require(material_bytes == path_in_repo('agent/conversation.json').read_bytes(), 'Material mirrors differ')
    require(material_bytes.lstrip(b' \t\r\n').startswith(b'{') and
            len(material_bytes) <= 128 * 1024, 'Material encoding/size')
    for name, expected in files.items():
        path = path_in_repo(name)
        require(path.stat().st_size == expected['bytes'] and sha(path) == expected['sha256'], name + ' digest')
    current = read_json(path_in_repo('conversation.json'))
    require(isinstance(current, dict), 'Material must be a JSON object')
    history_records = current
    if current.get('schema_version') == 'minivectordb-learning-evidence-curated-v1':
        selection = binding['selection_archive']
        archive_path = path_in_repo(selection['path'])
        require(archive_path.stat().st_size == selection['bytes'] and
                sha(archive_path) == selection['sha256'], 'Selection archive digest')
        history_records = read_json(archive_path)
        verify_selection(current, history_records)
    history = binding['before_material']
    previous_path = path_in_repo(history['path'])
    require(sha(previous_path) == history['sha256'] and previous_path.stat().st_size == history['bytes'], 'History digest')
    previous = read_json(previous_path)
    preserve_prefix(history_records, previous, 'Original measurement material')
    revision = binding['revision_baseline']
    revision_path = path_in_repo(revision['path'])
    require(revision_path.stat().st_size == revision['bytes'] and
            sha(revision_path) == revision['sha256'], 'Revision baseline digest')
    require(revision_path.read_bytes() == git_file(revision['commit'], 'agent/conversation.json'),
            'Revision baseline differs from its published commit')
    previous_revision = read_json(revision_path)
    preserve_prefix(history_records, previous_revision, 'Published revision material')
    if 'previous_formal_material' in binding:
        formal = binding['previous_formal_material']
        formal_path = path_in_repo(formal['path'])
        require(formal_path.stat().st_size == formal['bytes'] and
                sha(formal_path) == formal['sha256'], 'Formal baseline material digest')
        # The API-authored public commit and the local commit share an exact
        # tree. A fresh public clone need not contain the local commit object.
        verified = False
        for commit in [formal['commit'], formal['local_equivalent_commit']]:
            require(re.fullmatch(r'[0-9a-f]{40}', commit) is not None, 'Invalid formal source commit')
            tree = subprocess.run(['git', 'rev-parse', '--verify', commit + '^{tree}'],
                                  cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=30)
            if tree.returncode != 0:
                continue
            require(tree.stdout.decode('ascii').strip() == formal['shared_tree'], 'Formal source tree differs')
            require(formal_path.read_bytes() == git_file(commit, 'agent/conversation.json'),
                    'Formal baseline differs from its published tree')
            verified = True
            break
        require(verified, 'Formal baseline Git tree unavailable')
        preserve_prefix(history_records, read_json(formal_path), 'Latest formal material')
    count = len(previous_revision['conversations'])
    for field in ['conversations', 'experiments', 'agent_observations']:
        ids = [row['id'] for row in current[field]]
        require(len(ids) == len(set(ids)), 'Duplicate record IDs: ' + field)
    counts = binding['counts']
    require(counts == {'user_records':len(current['conversations']),
                       'experiments':len(current['experiments']),
                       'agent_observations':len(current['agent_observations']),
                       'retrospective_cases':len(current['case_studies'])}, 'Incorrect record counts')
    require(isinstance(current['scope'], str) and current['scope'], 'Missing coverage description')
    require(current['future_task_prompt']['source'] == 'agent_authored' and
            current['future_task_prompt']['status'] == 'new_template_not_executed', 'Template attribution')
    for section in ['material_overview', 'prompt_requirement_evolution']:
        require(current[section]['source'] == 'agent_retrospective', 'Retrospective attribution')
    for case in current['case_studies']:
        require(case['source'] == 'agent_retrospective', 'Case attribution')
        for name in case['files']:
            path_in_repo(name)

    reference_count = check_references(current)
    historical_chain = [f'e{i}' for i in range(57,63)]
    require(any([r.get('id') for r in item.get('result_evidence', [])] == historical_chain
                for item in current['review_navigation']['items']), 'Missing historical e57-e62 chain')

    measurement = binding['measurement_evidence']
    manifest_path = path_in_repo(measurement['path'])
    require(sha(manifest_path) == measurement['sha256'], 'Historical measurement manifest changed')
    manifest = read_json(manifest_path)
    require(manifest_path.read_bytes() == path_in_repo('bench/results/prompt-material-20261111/measurement-manifest-before.json').read_bytes(), 'Historical manifest snapshot differs')
    require(manifest['learning_sha256'] == history['sha256'] == measurement['material_at_measurement_sha256'], 'Historical material binding differs')
    source_commit = measurement['source_commit']
    require(source_commit == HISTORICAL_SOURCE_COMMIT, 'Historical measurement commit changed')
    for name, expected in manifest['source_sha256'].items():
        require(hashlib.sha256(git_file(source_commit, name)).hexdigest() == expected,
                'Historical measured source differs: ' + name)
    for name, expected in manifest['files'].items():
        path = manifest_path.parent / name
        require(is_within(path.resolve(), manifest_path.parent.resolve()), 'Invalid evidence path')
        require(sha(path) == expected, 'Historical evidence changed: ' + name)
    for content in [material_bytes.decode('utf-8'), json.dumps(history_records, ensure_ascii=False), previous_path.read_text(encoding='utf-8'),
                    revision_path.read_text(encoding='utf-8')]:
        require('github_pat_' not in content and 'ghp_' not in content, 'Credential marker in material')
        require(re.search(r'https?://[^/\s:]+:[^/\s@]+@', content) is None, 'Credential URL in material')
    print(json.dumps({'passed':True, 'bytes':len(material_bytes), 'sha256':sha(path_in_repo('conversation.json')),
                      'reference_checks':reference_count, 'preserved_user_records':count,
                      'preserved_experiments':len(previous_revision['experiments']),
                      'preserved_agent_observations':len(previous_revision['agent_observations']),
                      'source_hashes':len(manifest['source_sha256']), 'evidence_hashes':len(manifest['files']),
                      'historical_source_commit':source_commit, 'current_source_validation':'not performed by this script',
                      'platform_evaluation':'not performed by this script; user-reported result remains distinct'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
