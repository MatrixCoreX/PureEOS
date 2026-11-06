"""One reversible static release, preserving the existing server and all services."""
from pathlib import Path
import datetime,json,hashlib,os,shutil
import ops_common as o
R=o.R;S=R/'web-qualification-3/web/dist';root=Path('/var/www/aic-test');current=root/'current'
assert not(R/'FRONTEND_PUBLICATION.json').exists(),'Reconcile existing publication'
assert json.loads((R/'WEB_QUALIFIED.json').read_text())['status']=='PASS'
assert json.loads((R/'UI_PREVIEW.json').read_text())['status']=='PASS'
assert json.loads((R/'VISUAL_REVIEW_PREVIEW.json').read_text())['status']=='PASS'
assert str(current.resolve())=='/var/www/aic-test/releases/20261106T035606Z-proposal-tabs-r1','Another release changed; do not overwrite it'
manifest=json.loads((R/'WEB_QUALIFIED.json').read_text())['dist_files']
for n,h in manifest.items():assert o.sha((S/n).read_bytes())==h,n
o.guard();previous=str(current.resolve());stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-key-growth-r1';release=root/'releases'/stamp;assert not release.exists();shutil.copytree(S,release)
for p in[release,*release.rglob('*')]:p.chmod(0o755 if p.is_dir()else 0o644)
for n,h in manifest.items():assert o.sha((release/n).read_bytes())==h
link=root/('.next-'+stamp);link.symlink_to(release);os.replace(link,current)
o.save(R/'FRONTEND_PUBLICATION.json',{'status':'PUBLISHED_AWAITING_NORMAL_PUBLIC_QA','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_commit':'44f52ac0703e739f0e0aba661df0f9d441acfeff','previous_release':previous,'release':str(release),'files':manifest,'old_and_new_KEY_compatible':True,'no_server_restart_or_config_change':True});print('KEY_GROWTH_STATIC_RELEASE_PUBLISHED '+stamp)
