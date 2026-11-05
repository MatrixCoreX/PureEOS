"""Publish only exact qualified static caption assets after real preview QA."""
from pathlib import Path
import json,hashlib,datetime,shutil,os
R=Path('/opt/pureeos-bp-demo-six-20261105-r1');S=R/'frontend-final/dist'
assert not(R/'FRONTEND_PUBLICATION.json').exists()
for n in ['FRONTEND_FINAL_RESULT.json','CAPACITY_PREVIEW.json','CAPACITY_VISUAL_REVIEW.json']:assert json.loads((R/n).read_text())['status']=='PASS'
build=json.loads((R/'FRONTEND_FINAL_RESULT.json').read_text());assert build['source_SHA256']==hashlib.sha256((R/'frontend-final-source.tar.gz').read_bytes()).hexdigest()
for n,h in build['files'].items():assert hashlib.sha256((S/n).read_bytes()).hexdigest()==h
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-next-capacity-time';root=Path('/var/www/aic-test');previous=str((root/'current').resolve());release=root/'releases'/stamp
assert not release.exists();shutil.copytree(S,release)
for p in [release,*release.rglob('*')]:p.chmod(0o755 if p.is_dir()else 0o644)
for n,h in build['files'].items():assert hashlib.sha256((release/n).read_bytes()).hexdigest()==h
link=root/('.next-'+stamp);link.symlink_to(release);os.replace(link,root/'current')
(R/'FRONTEND_PUBLICATION.json').write_text(json.dumps({'status':'PUBLISHED_AWAITING_PUBLIC_QA','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'previous_release':previous,'release':str(release),'source_SHA256':build['source_SHA256'],'files':build['files'],'scope':'Static capacity-caption change only. No RPC/service/chain/contract changes.'},indent=2)+'\n');print('NEXT_CAPACITY_TIME_STATIC_PUBLISHED',stamp)
