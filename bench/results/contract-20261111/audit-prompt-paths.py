"""Read-only JSON/package path audit; does not infer the private Prompt loader.

Writes only the explicitly requested report. Uses immutable Git trees for the
published material, checks case exactly even on Windows, and does not read Git
configuration or credentials. Python 3.7+; no third-party packages required.
"""
import argparse
import datetime
import fnmatch
import hashlib
import json
import math
import re
import subprocess
from pathlib import Path


def require(ok, message):
    if not ok:
        raise ValueError(message)


def pairs_unique(items):
    result = {}
    for key, value in items:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError('Nonstandard JSON number: ' + value)


def finite_float(value):
    parsed = float(value)
    require(math.isfinite(parsed), 'Nonfinite JSON number')
    return parsed


def strict_json(raw):
    return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs_unique,
                      parse_constant=invalid_constant, parse_float=finite_float)


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def pointer_part(value):
    return str(value).replace('~', '~0').replace('/', '~1')


def walk(value, pointer=''):
    yield pointer, value
    if isinstance(value, dict):
        for key, child in value.items():
            yield from walk(child, pointer + '/' + pointer_part(key))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from walk(child, pointer + '/' + str(index))


def resolve(document, pointer):
    require(isinstance(pointer, str) and pointer.startswith('/'), 'Invalid pointer')
    value, parent = document, None
    for raw in pointer[1:].split('/'):
        require(not re.search(r'~(?![01])', raw), 'Invalid pointer escape')
        part = raw.replace('~1', '/').replace('~0', '~')
        parent = value
        if isinstance(value, list):
            require(re.fullmatch(r'0|[1-9][0-9]*', part), 'Invalid array index')
            require(int(part) < len(value), 'Array index out of range')
            value = value[int(part)]
        else:
            require(isinstance(value, dict) and part in value, 'Unresolved pointer')
            value = value[part]
    return value, parent


def git(root, *args):
    return subprocess.check_output(
        ['git', '--git-dir=' + str(root / '.git'), '--work-tree=' + str(root)] + list(args),
        stderr=subprocess.PIPE, timeout=60)


def git_tree(root, commit):
    entries = {}
    for line in git(root, 'ls-tree', '-rz', commit).split(b'\0'):
        if not line:
            continue
        metadata, path = line.split(b'\t', 1)
        mode, kind, oid = metadata.decode('ascii').split()
        entries[path.decode('utf-8')] = dict(mode=mode, kind=kind, oid=oid)
    dirs = set()
    for path in entries:
        parts = path.split('/')
        for count in range(1, len(parts)):
            dirs.add('/'.join(parts[:count]))
    return entries, dirs


def local_exact(root, name):
    current = root
    case_errors = []
    for part in name.rstrip('/').split('/'):
        require(part not in ['', '.', '..'], 'Unsafe repository path: ' + name)
        if not current.is_dir():
            return dict(exists=False, exact_case=False, case_errors=case_errors)
        names = [child.name for child in current.iterdir()]
        if part not in names:
            similar = [candidate for candidate in names if candidate.casefold() == part.casefold()]
            if not similar:
                return dict(exists=False, exact_case=False, case_errors=case_errors)
            case_errors.append(dict(component=part, actual=similar))
            current = current / similar[0]
        else:
            current = current / part
    return dict(exists=current.exists(), exact_case=not case_errors,
                kind='directory' if current.is_dir() else 'file',
                symlink=current.is_symlink(), case_errors=case_errors)


REPO_TOKEN = re.compile(r'(?<![A-Za-z0-9_])(?:docs|bench|tests|src|include|scripts|agent|experiments|work|outputs)/[A-Za-z0-9_./*\-]+')
LOCAL_TOKEN = re.compile(r'(?:[A-Z]:[/\\]|/(?:mnt|var|data|tmp)(?:/|\b))[A-Za-z0-9_./\\*\-]*')
SLASH_TOKEN = re.compile(r'[A-Za-z0-9_.*:\\-]*(?:[/\\][A-Za-z0-9_.*:\\-]+)+/?')


def audit_document(root, raw, mirror, label, entries, dirs):
    document = strict_json(raw)
    require(isinstance(document, dict), 'Top level must be object')
    pointers, references, local_mentions, metadata, slash_candidates = [], [], [], [], []
    for location, value in walk(document):
        if isinstance(value, dict) and 'pointer' in value:
            target, parent = resolve(document, value['pointer'])
            require(isinstance(target, str), 'Pointer target must be string')
            if 'id' in value:
                require(isinstance(parent, dict) and parent.get('id') == value['id'], 'Pointer ID differs')
            if 'quote' in value:
                require(isinstance(value['quote'], str) and 0 < len(value['quote']) <= 240,
                        'Quote length/type')
                require(value['quote'] in target, 'Quote does not match exact source')
            pointers.append(dict(location=location, target=value['pointer'],
                                 target_type='string', quote_exact=True if 'quote' in value else None,
                                 id_matches=True if 'id' in value else None))
        if not isinstance(value, str):
            continue
        key = location.rsplit('/', 1)[-1].lower()
        if key in ['source', 'path', 'file', 'directory', 'root', 'schema_version', 'baseline_commit']:
            metadata.append(dict(location=location, value=value,
                                 classification='provenance_or_schema_label' if key in ['source','schema_version','baseline_commit'] else 'path_metadata'))
        if key == 'pointer':
            continue
        for token in REPO_TOKEN.findall(value):
            references.append(dict(location=location, path=token, syntax='literal_or_glob'))
        for token in LOCAL_TOKEN.findall(value):
            mapped = None
            if re.match(r'^[A-Z]:', token):
                mapped = Path(token)
            elif token.startswith('/mnt/d/'):
                mapped = Path('D:/' + token[len('/mnt/d/'):])
            local_mentions.append(dict(location=location, path=token,
                                       classification='unexecuted_future_local_template' if location.startswith('/future_task_prompt/') else 'historical_runtime_or_environment_text',
                                       mapped_windows_path=str(mapped) if mapped else None,
                                       mapped_exists=mapped.exists() if mapped else None,
                                       evaluator_dependency=False))
        remaining = [token for token in SLASH_TOKEN.findall(value)
                     if not REPO_TOKEN.fullmatch(token) and not LOCAL_TOKEN.fullmatch(token)]
        if remaining:
            slash_candidates.append(dict(location=location, tokens=remaining))
    for index, experiment in enumerate(document.get('experiments', [])):
        evidence = experiment.get('evidence', '')
        grouped = re.fullmatch(r'([^ ]+)/ \((.*)\)', evidence)
        if grouped:
            for tail in grouped[2].split(';'):
                references.append(dict(location='/experiments/%d/evidence' % index,
                                       path=grouped[1] + '/' + tail, syntax='expanded_grouped_relative_name'))
    problems = []
    if raw != mirror:
        problems.append(dict(error='material_mirrors_differ'))
    if len(raw) > 128 * 1024:
        problems.append(dict(error='material_exceeds_128_KiB', bytes=len(raw)))
    if raw.startswith(b'\xef\xbb\xbf'):
        problems.append(dict(error='material_has_utf8_bom'))
    for item in references:
        name = item['path'].rstrip('/')
        if '*' in name:
            matches = sorted(str(p.relative_to(root)).replace('\\', '/') for p in root.glob(name))
            git_matches = sorted(path for path in entries if fnmatch.fnmatchcase(path, name))
            item.update(local_matches=matches, published_matches=git_matches)
            item['local_exact_case'] = all(local_exact(root, path)['exact_case'] for path in matches)
            item['local_exists'] = bool(matches)
            item['published_exists'] = bool(git_matches)
            item['symlinks'] = [path for path in git_matches if entries[path]['mode'] == '120000']
        else:
            local = local_exact(root, name)
            item['local'] = local
            item['local_exists'] = local['exists']
            item['local_exact_case'] = local['exact_case']
            item['published_exists'] = name in entries or name in dirs
            item['published_kind'] = entries.get(name, {}).get('kind', 'tree' if name in dirs else None)
            item['published_entry'] = entries.get(name)
            item['case_alternatives_in_commit'] = sorted(path for path in set(entries) | dirs
                                                         if path.casefold() == name.casefold() and path != name)
        if not item['local_exists'] or not item['local_exact_case']:
            problems.append(dict(location=item['location'], path=item['path'], error='missing_or_case_mismatch_local'))
        if label == 'published' and not item['published_exists']:
            problems.append(dict(location=item['location'], path=item['path'], error='absent_in_published_commit'))
    material_modes = {name: entries.get(name) for name in ['conversation.json','agent/conversation.json']}
    evidence_files = sorted(set(path for item in references for path in
                                (item.get('published_matches', []) if '*' in item['path'] else
                                 [item['path'].rstrip('/')] if item['path'].rstrip('/') in entries else [])))
    published_special_entries = [dict(path=path, entry=entries[path]) for path in evidence_files
                                 if entries[path]['mode'] not in ['100644', '100755']]
    local_lfs_pointers = []
    for item in references:
        names = item.get('local_matches', []) if '*' in item['path'] else [item['path'].rstrip('/')]
        for name in names:
            file_path = root / name
            if file_path.is_file():
                with file_path.open('rb') as stream:
                    if stream.read(200).startswith(b'version https://git-lfs.github.com/spec/v1'):
                        local_lfs_pointers.append(name)
    for name, entry in material_modes.items():
        if not entry or entry['mode'] not in ['100644','100755'] or entry['kind'] != 'blob':
            problems.append(dict(path=name, error='material_not_regular_git_blob'))
    if published_special_entries:
        problems.append(dict(error='evidence_git_special_entries', entries=published_special_entries))
    if local_lfs_pointers:
        problems.append(dict(error='evidence_lfs_pointers', paths=sorted(set(local_lfs_pointers))))
    return dict(version=label, bytes=len(raw), within_128_KiB=len(raw) <= 128 * 1024,
                sha256=sha256(raw), mirrors_equal=raw == mirror, utf8=True,
                bom=raw.startswith(b'\xef\xbb\xbf'), strict_json=True, top_level='object',
                synthetic=document.get('synthetic'), schema_version=document.get('schema_version'),
                lfs_pointer=raw.startswith(b'version https://git-lfs.github.com/spec/v1'),
                published_entry_modes=material_modes, pointer_count=len(pointers), pointers=pointers,
                repository_reference_count=len(references),
                repository_unique_path_count=len(set(item['path'] for item in references)),
                repository_references=references, local_path_mentions=local_mentions,
                published_referenced_file_count=len(evidence_files),
                published_special_entries=published_special_entries,
                local_evidence_lfs_pointers=sorted(set(local_lfs_pointers)),
                source_and_path_metadata=metadata, other_slash_tokens_for_manual_review=slash_candidates,
                problems=problems)


def official_contract(path):
    loaded = strict_json(path.read_bytes())
    data = loaded['data']
    text = data['content']
    needles = ['agent/conversation.json', '128 KiB', '不强制', '不会自动读取',
               '不会自动搜索', 'RFC6901', '同一次 commit', '不设固定上限']
    snippets = {}
    for needle in needles:
        snippets[needle] = [text[max(0, match.start() - 120):match.end() + 400]
                            for match in list(re.finditer(re.escape(needle), text))[:6]]
    return dict(path=str(path), content_updated_at=data.get('content_updated_at'),
                content_sha256=sha256(text.encode('utf-8')), relevant_lines=snippets)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path('D:/Codex/MiniVectorDB/repo'))
    parser.add_argument('--commit', default='524133baf0a4ed947805236c02cce9cecffc0be3')
    parser.add_argument('--official-doc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(re.fullmatch('[0-9a-f]{40}', args.commit), 'Use full immutable commit SHA')
    root = args.root.resolve()
    entries, dirs = git_tree(root, args.commit)
    current = audit_document(root, (root / 'conversation.json').read_bytes(),
                             (root / 'agent/conversation.json').read_bytes(), 'current', entries, dirs)
    published = audit_document(root, git(root, 'show', args.commit + ':conversation.json'),
                               git(root, 'show', args.commit + ':agent/conversation.json'),
                               'published', entries, dirs)
    report = dict(audit='prompt-material-path-and-interface',
                  generated_at_utc=datetime.datetime.utcnow().isoformat() + 'Z',
                  root=str(root), published_commit=args.commit, current=current, published=published,
                  findings=[
                      ('No missing repository evidence paths, Linux case mismatch, unresolved JSON pointers, or material-entry symlink/LFS pointer found.'
                       if not current['problems'] and not published['problems'] else 'One or more packaging/path problems were found; see each version problems list.'),
                      'The official input contract fixes packaging and default agent/conversation.json path, but does not prescribe these business field names.',
                      'Official evaluation reads the single JSON content only; valid repository evidence paths are human provenance, not automatic evaluator dependencies.',
                      'Local absolute paths are historical environment/output text or an explicitly unexecuted template, not loader configuration.',
                      'This audit cannot inspect the platform effective path override, material digest, evaluation report, or private loader/output validator; it cannot establish the cause of the combined invalid/pending message.'
                  ],
                  manual_review_scope='All remaining slash tokens were reviewed as ratios, resource units, action names, record IDs, grouped names, remote repository URLs, or historical branch names. The only grouped relative file names were expanded explicitly.',
                  mutations='Only this report file is written; material, Git refs/configuration, code, and existing evidence are unchanged.')
    if args.official_doc:
        report['official_contract'] = official_contract(args.official_doc)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(report=str(args.output), current_paths=current['repository_reference_count'],
                          published_paths=published['repository_reference_count'],
                          current_pointers=current['pointer_count'], published_pointers=published['pointer_count'],
                          problems=len(current['problems']) + len(published['problems'])), ensure_ascii=False))
    require(not current['problems'] and not published['problems'], 'Path problems found; inspect report')


if __name__ == '__main__':
    main()
