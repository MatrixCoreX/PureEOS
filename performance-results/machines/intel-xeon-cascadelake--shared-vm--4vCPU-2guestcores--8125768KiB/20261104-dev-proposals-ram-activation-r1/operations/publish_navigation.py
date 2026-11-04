"""Publish the user's CSS-only navigation change during the native review."""
from pathlib import Path
import hashlib,json,subprocess
K=Path('/opt/pureeos-dev-proposals-20261104-r1');before=json.loads((K/'DEPLOYMENT.json').read_text());after=json.loads((K/'FRONTEND_RESULT.json').read_text())
assert json.loads((K/'browser-public-review/RESULT.json').read_text())['status']=='PASS'
assert json.loads((K/'navigation-preview/RESULT.json').read_text())['build_files']==after['files']
assert before['status']=='PUBLISHED'and Path('/var/www/aic-test/current').resolve()==Path(before['release'])
for suffix in ['.js','.svg']:
 assert sorted(v for n,v in before['files'].items()if n.endswith(suffix))==sorted(v for n,v in after['files'].items()if n.endswith(suffix)),'Functional artifacts changed'
assert all(hashlib.sha256((Path(before['release'])/n).read_bytes()).hexdigest()==v for n,v in before['files'].items())
for name in ['DEPLOYMENT','PUBLISH_INTENT']:
 p=K/(name+'.json');archive=K/(name+'_r2.json');assert not archive.exists();p.rename(archive)
subprocess.run(['python3','-B',str(K/'publish_frontend.py'),'navigation-preview'],check=True)
