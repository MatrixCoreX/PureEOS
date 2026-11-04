"""Copy the already-qualified SDK/relay; do not start or switch services."""
from pathlib import Path
import hashlib,json,shutil
K=Path('/opt/pureeos-dev-proposals-20261104-r1');Q=Path('/opt/pureeos-key-ram-market-20261104-r1');S=Q/'source-r3';T=K/'runtime'
assert json.loads((Q/'QUALIFICATION_native_r3.json').read_text())['status']=='PASS'
assert not T.exists();T.mkdir()
for name in ['sdk','relayer']:shutil.copytree(S/name,T/name)
for name in ['package.json','package-lock.json']:shutil.copyfile(S/name,T/name)
deps=Path('/opt/pureeos-keynet-20261103-r2/key-service/node_modules')
assert json.loads((deps/'@wharfkit/antelope/package.json').read_text())['version']=='4.0.2'
(T/'node_modules').symlink_to(deps,target_is_directory=True)
files={str(p.relative_to(T)):hashlib.sha256(p.read_bytes()).hexdigest()for p in T.rglob('*')if p.is_file()and 'node_modules'not in p.parts}
assert all(hashlib.sha256((S/n).read_bytes()).hexdigest()==v for n,v in files.items())
(K/'RUNTIME_PREPARED.json').write_text(json.dumps({'status':'PREPARED','qualified_source':str(S),'runtime':str(T),'files':files,'services_changed':False},indent=2)+'\n')
print('Qualified runtime copied; services unchanged')
