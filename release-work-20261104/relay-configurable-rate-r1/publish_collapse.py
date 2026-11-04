from pathlib import Path
import datetime,hashlib,json,shutil
F=Path('/opt/pureeos-explorer-key-rate-20261104-r4-collapse')
OLD=Path('/opt/pureeos-explorer-key-rate-20261104-r3')
K=Path('/opt/pureeos-key-rate-20261104-r1')
B=json.loads((F/'BUILD_RESULT.json').read_text());assert B['status']=='PASS'
P=json.loads((F/'browser-preview/RESULT.json').read_text());assert P['status']=='PASS' and P['build_files']==B['files']
assert len(P['variants'])==4 and all(v['formula_default_collapsed'] and v['formula_expand_collapse'] for v in P['variants'])
assert json.loads((F/'INTEGRATION_RESULT.json').read_text())['SDK_comparisons']==60
assert json.loads((K/'LIVE_SMOKE.json').read_text())['status']=='PASS'
digest=lambda root:{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
root=Path('/var/www/aic-test');current=root/'current';old=current.resolve()
previous=json.loads((OLD/'DEPLOYMENT.json').read_text())
assert str(old)==previous['release'] and digest(old)==previous['files']
source=F/'source/dist';assert digest(source)==B['files']
release=root/'releases'/(datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-fee-formula-collapse')
shutil.copytree(source,release)
for p in [release,*release.rglob('*')]:p.chmod(0o755 if p.is_dir()else 0o644)
link=root/'current-fee-formula-collapse-next';assert not link.exists() and not link.is_symlink()
link.symlink_to(release);link.replace(current)
result={'status':'DEPLOYED','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'release':str(release),'rollback':str(old),'files':B['files'],
        'scope':'Formula and billing notes inside existing default-closed disclosure; quote and relay selection stay visible',
        'new_contract_change':False,'default_collapsed':True}
(F/'DEPLOYMENT.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'status':result['status'],'release':str(release)}))
