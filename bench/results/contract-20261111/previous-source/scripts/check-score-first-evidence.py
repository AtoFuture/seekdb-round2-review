"""Check this working-tree experiment's source/evidence binding, not a score."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
folder = root/'bench/results/score-first-20261111'
manifest = json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def contained(base, name):
    path = (base/name).resolve()
    path.relative_to(base.resolve())
    if not path.is_file():
        raise ValueError('Missing file: '+name)
    return path
for name, expected in manifest['source_sha256'].items():
    if digest(contained(root, name)) != expected:
        raise ValueError('Current source differs from measured candidate: '+name)
for name, expected in manifest['evidence_sha256'].items():
    if digest(contained(folder, name)) != expected:
        raise ValueError('Archived evidence differs: '+name)
image = json.loads((folder/'score-first-image-source.json').read_text(encoding='utf-8'))
for name, expected in image['source_sha256'].items():
    if manifest['source_sha256'].get(name) != expected:
        raise ValueError('Test image source differs: '+name)
if (folder/'score-first-runtime-binary.txt').read_text(encoding='utf-8').split()[0] != image['binary_sha256']:
    raise ValueError('Runtime/test binaries differ')
binding = json.loads((root/'agent/material-digest.json').read_text(encoding='utf-8'))
expected = manifest['learning_sha256']
for name in ['conversation.json','agent/conversation.json']:
    if digest(root/name) != expected or binding['material_files'][name]['sha256'] != expected:
        raise ValueError('Current learning material binding differs: '+name)
print(json.dumps({'passed':True, 'source_files':len(manifest['source_sha256']),
                  'evidence_files':len(manifest['evidence_sha256']),
                  'runtime_binary_sha256':image['binary_sha256'],
                  'learning_sha256':expected, 'official_score':None}))
