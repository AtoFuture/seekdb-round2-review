#!/usr/bin/env python3
"""Read-only local packaging preflight for the published 4.4 material contract.

Python 3.7+, standard library only. The material's business schema is free.
The root conversation.json in the public repository is an empty placeholder,
not a required template. This tool does not call the competition service or
produce its Prompt score, digest, or evaluation status.

Examples:
  python3 -B scripts/check-submission-contract.py --repository .
  python3 -B scripts/check-submission-contract.py --repository . --ref FULL_SHA
  python3 -B scripts/check-submission-contract.py --check-internal-references

Exit 0 means local packaging checks passed; exit 1 means a reported check
failed. A present root mirror must match byte-for-byte (a project policy,
not an official requirement). --check-internal-references is also optional
project policy and only checks objects containing both "pointer" and "quote".
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


MAX_BYTES = 128 * 1024
REGULAR_MODES = ('100644', '100755')


class CheckError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class NumberToken:
    """Accept JSON numeric syntax without imposing float range or int limits."""
    def __init__(self, value):
        self.value = value


def safe_parts(path):
    parts = path.split('/')
    if (not path or '\\' in path or ':' in path or '\x00' in path or
            any(part in ('', '.', '..') for part in parts)):
        raise CheckError('unsafe_path', 'Use an exact repository-relative path with forward slashes; '
                         'absolute paths, dot segments, empty segments and backslashes are rejected.')
    return parts


def choose_exact(names, part, path):
    if part in names:
        return
    if any(name.casefold() == part.casefold() for name in names):
        raise CheckError('case_mismatch', 'Path case does not match the stored entry: ' + path)
    raise CheckError('missing_path', 'Requested material entry does not exist: ' + path)


def git(repository, *arguments):
    environment = os.environ.copy()
    # A replace ref must not silently change what an immutable SHA identifies.
    environment['GIT_NO_REPLACE_OBJECTS'] = '1'
    try:
        result = subprocess.run(['git', '-c', 'core.bare=false'] + list(arguments),
                                cwd=str(repository), env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CheckError('git_read_error', 'Could not read Git objects: ' + str(error))
    if result.returncode:
        raise CheckError('git_read_error', result.stderr.decode('utf-8', errors='replace').strip())
    return result.stdout


def validate_ref(repository, ref):
    if re.fullmatch(r'[0-9a-fA-F]{40}', ref) is None:
        raise CheckError('invalid_ref', '--ref requires a complete 40-hex commit SHA.')
    try:
        kind = git(repository, 'cat-file', '-t', ref).strip()
    except CheckError as error:
        raise CheckError('invalid_ref', 'Commit is not readable in this repository: ' + str(error))
    if kind != b'commit':
        raise CheckError('invalid_ref', '--ref must identify a commit object, not a tag/tree/blob.')
    return ref.lower()


def identify_bytes(data, identity):
    identity['bytes'] = len(data)
    identity['sha256'] = hashlib.sha256(data).hexdigest()
    if identity.get('git_blob') is None:
        # Content identity only: this does not imply the worktree bytes are committed.
        identity['git_blob'] = hashlib.sha1(
            b'blob ' + str(len(data)).encode('ascii') + b'\x00' + data).hexdigest()
    return data


def read_worktree(repository, path, identity):
    current = repository
    parts = safe_parts(path)
    for index, part in enumerate(parts):
        choose_exact([item.name for item in current.iterdir()], part, path)
        current = current / part
        info = current.lstat()
        # Reject symlinks and Windows reparse points, including parent junctions.
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise CheckError('not_regular_file', 'Symlink/reparse entries are not material: ' + path)
        if index < len(parts) - 1 and not stat.S_ISDIR(info.st_mode):
            raise CheckError('not_regular_file', 'Material parent is not a directory: ' + path)
    identity['resolved_path'] = str(current.resolve())
    if not stat.S_ISREG(info.st_mode):
        raise CheckError('not_regular_file', 'Material must be a regular file: ' + path)
    identity['bytes'] = info.st_size
    if info.st_size > MAX_BYTES:
        raise CheckError('size_limit', 'Material exceeds 128 KiB (131072 bytes).')
    with current.open('rb') as handle:
        data = handle.read(MAX_BYTES + 1)
    identify_bytes(data, identity)
    if len(data) > MAX_BYTES:
        raise CheckError('size_limit', 'Material exceeds 128 KiB (131072 bytes).')
    return data


def read_commit(repository, ref, path, identity):
    tree = ref
    parts = safe_parts(path)
    for index, part in enumerate(parts):
        entries = {}
        for row in git(repository, 'ls-tree', '-z', tree).split(b'\x00'):
            if not row:
                continue
            metadata, name = row.split(b'\t', 1)
            mode, kind, oid = metadata.decode('ascii').split()
            entries[name.decode('utf-8', errors='surrogateescape')] = (mode, kind, oid)
        choose_exact(entries, part, path)
        mode, kind, oid = entries[part]
        if index < len(parts) - 1:
            if mode != '040000' or kind != 'tree':
                raise CheckError('not_regular_file', 'Git material parent is not a tree: ' + path)
            tree = oid
    identity.update(git_path=path, git_blob=oid, git_mode=mode)
    if mode not in REGULAR_MODES or kind != 'blob':
        raise CheckError('not_regular_file', 'Git material must have regular mode 100644/100755: ' + path)
    identity['bytes'] = int(git(repository, 'cat-file', '-s', oid).strip())
    if identity['bytes'] > MAX_BYTES:
        raise CheckError('size_limit', 'Material exceeds 128 KiB (131072 bytes).')
    return identify_bytes(git(repository, 'cat-file', 'blob', oid), identity)


def strict_json(data):
    if data.startswith(b'version https://git-lfs.github.com/spec/v1'):
        raise CheckError('lfs_pointer', 'Material is a Git LFS pointer, not the JSON bytes.')
    if data.startswith(b'\xef\xbb\xbf'):
        raise CheckError('utf8_bom', 'UTF-8 BOM is not allowed.')
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError as error:
        raise CheckError('invalid_utf8', str(error))

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate JSON key: ' + key)
            result[key] = value
        return result

    def constant(token):
        raise ValueError('Non-standard JSON constant: ' + token)

    try:
        document = json.loads(text, object_pairs_hook=unique, parse_constant=constant,
                              parse_float=NumberToken, parse_int=NumberToken)
        if not isinstance(document, dict):
            raise ValueError('Material must be a JSON object at the top level.')
        return document
    except (ValueError, RecursionError) as error:
        raise CheckError('invalid_json', str(error))


def resolve_pointer(document, pointer):
    if not isinstance(pointer, str) or (pointer != '' and not pointer.startswith('/')):
        raise ValueError('Expected a JSON pointer string.')
    value = document
    for raw in pointer[1:].split('/') if pointer else []:
        if re.search(r'~(?![01])', raw):
            raise ValueError('Invalid JSON pointer escape: ' + pointer)
        part = raw.replace('~1', '/').replace('~0', '~')
        if isinstance(value, list):
            if not re.fullmatch(r'0|[1-9][0-9]*', part) or len(part) > 12:
                raise ValueError('Invalid JSON pointer array index: ' + pointer)
            value = value[int(part)]
        elif isinstance(value, dict):
            value = value[part]
        else:
            raise ValueError('JSON pointer crosses a scalar: ' + pointer)
    return value


def check_internal_references(document, report):
    pending = [document]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            if 'pointer' in value and 'quote' in value:
                report['checked'] += 1
                try:
                    target = resolve_pointer(document, value['pointer'])
                    quote = value['quote']
                    if not isinstance(target, str) or not isinstance(quote, str) or not quote or quote not in target:
                        raise ValueError('Quote must be a nonempty substring of the pointed-to string.')
                except (ValueError, KeyError, IndexError, TypeError) as error:
                    report['pass'] = False
                    raise CheckError('internal_reference', str(error))
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    report['pass'] = True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repository', default='.', help='Repository/worktree directory (default: current directory).')
    parser.add_argument('--material-path', default='agent/conversation.json', help='Exact repository-relative material entry.')
    parser.add_argument('--ref', help='Read only this complete 40-hex commit SHA; omit to read working-tree bytes.')
    parser.add_argument('--check-internal-references', action='store_true',
                        help='Optional local check of internal {pointer, quote} references; no required business schema.')
    arguments = parser.parse_args(argv)
    repository = Path(arguments.repository).resolve()
    report = dict(scope='local_packaging_only', local_packaging_only=True,
                  source='git_commit' if arguments.ref else 'working_tree',
                  repository=str(repository), requested_path=arguments.material_path,
                  resolved_path=None, git_path=None, ref=arguments.ref, bytes=None,
                  sha256=None, git_blob=None, git_mode=None, format_pass=False,
                  mirror={'scope': 'local_mirror_check', 'path': 'conversation.json', 'status': 'not_checked'},
                  internal_references={'scope': 'optional_local_check',
                                       'enabled': arguments.check_internal_references,
                                       'checked': 0, 'pass': None}, errors=[])

    def load(path, identity):
        return (read_commit(repository, report['ref'], path, identity) if report['ref'] is not None
                else read_worktree(repository, path, identity))

    def record(error, scope):
        report['errors'].append({'code': error.code, 'scope': scope, 'message': str(error)})

    try:
        safe_parts(arguments.material_path)
        if not repository.is_dir():
            raise CheckError('repository_missing', 'Repository directory does not exist.')
        if arguments.ref is not None:
            report['ref'] = validate_ref(repository, arguments.ref)
        data = load(arguments.material_path, report)
        mirror = report['mirror']
        if arguments.material_path == mirror['path']:
            mirror['status'] = 'same_as_material'
        else:
            try:
                mirror_data = load(mirror['path'], mirror)
                mirror['status'] = 'match' if mirror_data == data else 'mismatch'
                if mirror_data != data:
                    record(CheckError('mirror_mismatch', 'Optional root copy differs from the requested material.'),
                           'local_mirror_check')
            except CheckError as error:
                mirror['status'] = 'absent' if error.code == 'missing_path' else 'error'
                if error.code != 'missing_path':
                    record(error, 'local_mirror_check')
        document = strict_json(data)
        report['format_pass'] = True
        if arguments.check_internal_references:
            try:
                check_internal_references(document, report['internal_references'])
            except CheckError as error:
                record(error, 'optional_local_check')
    except CheckError as error:
        record(error, 'local_packaging_only')
    except (OSError, ValueError, RecursionError) as error:
        record(CheckError('read_error', str(error)), 'local_packaging_only')
    report['local_checks_pass'] = report['format_pass'] and not report['errors']
    print(json.dumps(report, ensure_ascii=True, indent=2, allow_nan=False))
    return 0 if report['local_checks_pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
