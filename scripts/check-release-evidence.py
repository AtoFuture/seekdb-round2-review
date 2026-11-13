"""Verify release evidence hashes and identity with the measured native source."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
m=json.loads((root/'bench/results/release-20261112/manifest.json').read_text())
for name,info in m['files'].items():
 p=(root/name).resolve();p.relative_to(root)
 data=p.read_bytes()
 assert len(data)==info['bytes'] and hashlib.sha256(data).hexdigest()==info['sha256'],name
def tree(ref,name):
 return subprocess.check_output(['git','rev-parse',ref+':'+name],cwd=root).decode().strip()
for name in ['src','include','tests','CMakeLists.txt','Dockerfile']:
 if name=='tests':
  # Material-only tests may change; individual compiled sources are fixed by the manifest.
  continue
 assert tree('HEAD',name)==tree(m['measured_engine_commit'],name),name
for name in m['measured_test_paths']:
 assert tree('HEAD',name)==tree(m['measured_engine_commit'],name),name
print(json.dumps({'passed':True,'files':len(m['files']),'measured_engine_commit':m['measured_engine_commit'],'compiled_tests_unchanged':len(m['measured_test_paths']),'official_score_claimed':False}))
