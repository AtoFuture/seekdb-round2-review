#!/usr/bin/env python3
"""Verify separate historical and current local evidence bindings (Python 3.7+).

Run from any directory. Repository root is the parent of this script's directory.
Historical sources/material come from contract-20261111/previous-source, laid out
as a repository. Historical evidence stays in score-first-20261111. This utility
verifies file integrity; it does not produce a competition result.
"""
import hashlib
import json
from pathlib import Path
import re
import stat
import sys

CURRENT_FOLDER = 'bench/results/contract-20261111'
PREVIOUS_FOLDER = 'bench/results/score-first-20261111'
SELF_PATH = 'scripts/check-contract-evidence.py'
MATERIAL_PATHS = ('conversation.json', 'agent/conversation.json')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def contained(base, name, directory=False):
    """Require exact relative paths; reject symlinks, junctions and traversal."""
    require(isinstance(name, str) and bool(name), 'Empty/non-string binding path')
    parts = name.split('/')
    require('\\' not in name and ':' not in name and '\x00' not in name and
            all(part not in ('', '.', '..') for part in parts), 'Unsafe binding path: ' + name)
    current = base
    for position, part in enumerate(parts):
        require(current.is_dir(), 'Missing binding directory: ' + str(current))
        require(part in [child.name for child in current.iterdir()],
                'Missing or incorrectly cased binding path: ' + name)
        current = current / part
        info = current.lstat()
        require(not stat.S_ISLNK(info.st_mode) and
                not (getattr(info, 'st_file_attributes', 0) & 0x400),
                'Symlink/reparse binding path: ' + name)
        if position < len(parts) - 1:
            require(stat.S_ISDIR(info.st_mode), 'Binding parent is not a directory: ' + name)
    current.resolve().relative_to(base.resolve())
    require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
            'Binding target has wrong file type: ' + name)
    return current


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def sha256(value, label):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None,
            'Expected lowercase SHA256: ' + label)
    return value


def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON key: ' + key)
            result[key] = value
        return result

    def invalid_constant(token):
        raise ValueError('Non-standard JSON constant: ' + token)

    result = json.loads(path.read_bytes().decode('utf-8'), object_pairs_hook=unique,
                        parse_constant=invalid_constant)
    require(isinstance(result, dict), 'Expected JSON object: ' + str(path))
    return result


def hash_map(manifest, field):
    mapping = manifest[field]
    require(isinstance(mapping, dict) and bool(mapping), 'Missing/non-object hash map: ' + field)
    for name, expected in mapping.items():
        sha256(expected, field + '/' + name)
    return mapping


def verify_map(base, mapping, label):
    for name, expected in mapping.items():
        require(digest(contained(base, name)) == expected, label + ' hash differs: ' + name)


def verify_material(base, expected):
    sha256(expected, 'learning_sha256')
    binding = read_json(contained(base, 'agent/material-digest.json'))
    files = binding['material_files']
    require(isinstance(files, dict), 'Material digest must contain material_files')
    for name in MATERIAL_PATHS:
        path = contained(base, name)
        require(digest(path) == expected, 'Material bytes differ: ' + str(path))
        entry = files[name]
        require(isinstance(entry, dict) and entry.get('sha256') == expected,
                'Material digest SHA256 differs: ' + str(path))
        require(type(entry.get('bytes')) is int and entry['bytes'] == path.stat().st_size,
                'Material digest byte count differs: ' + str(path))


def verify_image(folder, manifest, prefix):
    source_name = prefix + '-image-source.json'
    runtime_name = prefix + '-runtime-binary.txt'
    evidence = manifest['evidence_sha256']
    require(source_name in evidence and runtime_name in evidence,
            'Image source and runtime binary records must be hashed as evidence')
    image = read_json(contained(folder, source_name))
    image_sources = hash_map(image, 'source_sha256')
    for name, expected in image_sources.items():
        require(manifest['source_sha256'].get(name) == expected,
                'Image source differs from measured source manifest: ' + name)
    binary = sha256(image['binary_sha256'], 'image binary_sha256')
    words = contained(folder, runtime_name).read_text(encoding='utf-8').split()
    require(bool(words) and words[0] == binary, 'Runtime/test image binary hashes differ')
    return binary


def check(repository, report):
    folder = contained(repository, CURRENT_FOLDER, directory=True)
    previous_folder = contained(repository, PREVIOUS_FOLDER, directory=True)
    manifest = read_json(contained(folder, 'manifest.json'))
    current_sources = hash_map(manifest, 'source_sha256')
    current_evidence = hash_map(manifest, 'evidence_sha256')
    require(SELF_PATH in current_sources, 'Current source hashes must include this checker')
    require('manifest.json' not in current_evidence, 'Evidence must exclude its own manifest')
    require(manifest['previous_source_directory'] == 'previous-source',
            'previous_source_directory must be previous-source')
    previous_source = contained(folder, manifest['previous_source_directory'], directory=True)

    # Freeze the prior manifest before trusting any of its internal hashes.
    previous_hash = sha256(manifest['previous_manifest_sha256'], 'previous_manifest_sha256')
    previous_manifest_path = contained(previous_folder, 'manifest.json')
    archived_manifest_path = contained(previous_source, PREVIOUS_FOLDER + '/manifest.json')
    require(digest(previous_manifest_path) == previous_hash,
            'Original historical manifest differs from its frozen hash')
    require(digest(archived_manifest_path) == previous_hash,
            'Archived historical manifest differs from its frozen hash')
    previous = read_json(previous_manifest_path)
    previous_sources = hash_map(previous, 'source_sha256')
    previous_evidence = hash_map(previous, 'evidence_sha256')
    require('manifest.json' not in previous_evidence, 'Historical evidence must exclude its own manifest')
    verify_map(previous_source, previous_sources, 'Archived historical source')
    verify_material(previous_source, previous['learning_sha256'])
    verify_map(previous_folder, previous_evidence, 'Historical evidence')
    report['historical_runtime_binary_sha256'] = verify_image(previous_folder, previous, 'score-first')
    report['historical_source_files'] = len(previous_sources)
    report['historical_evidence_files'] = len(previous_evidence)
    report['previous_manifest_sha256'] = previous_hash
    report['historical_evidence_pass'] = True

    verify_map(repository, current_sources, 'Current source')
    verify_map(folder, current_evidence, 'Current evidence')
    verify_material(repository, manifest['learning_sha256'])
    report['runtime_binary_sha256'] = verify_image(folder, manifest, 'contract-final')
    report['source_files'] = len(current_sources)
    report['evidence_files'] = len(current_evidence)
    report['learning_sha256'] = manifest['learning_sha256']
    report['current_source_match'] = True
    report['current_evidence_pass'] = True


def main():
    report = {'scope': 'local_evidence_integrity_only',
              'historical_evidence_pass': False, 'current_source_match': False,
              'current_evidence_pass': False, 'passed': False, 'errors': []}
    try:
        check(Path(__file__).resolve().parents[1], report)
        report['passed'] = True
    except (OSError, ValueError, KeyError, TypeError, RecursionError) as error:
        report['errors'].append(str(error))
    print(json.dumps(report, ensure_ascii=True, allow_nan=False, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
