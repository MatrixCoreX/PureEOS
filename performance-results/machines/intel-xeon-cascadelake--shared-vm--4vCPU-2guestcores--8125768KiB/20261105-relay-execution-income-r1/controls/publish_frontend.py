"""Publish reviewed static files only after actual candidate preview QA."""
from pathlib import Path
import datetime,hashlib,json,os,shutil,urllib.request
K=Path('/opt/pureeos-relay-execution-income-20261105-r1');S=K/'frontend-r3/dist'
assert not(K/'FRONTEND_PUBLICATION.json').exists()
for p in ['EXECUTION.json','APPS_SWITCHED.json','PUBLIC_TRANSFER_RESULT.json','FINAL_UI_PREVIEW_R3.json','VISUAL_REVIEW.json']:
    value=json.loads((K/p).read_text());assert value['status']in ['PASS','UPGRADED_IRREVERSIBLE'],p
b=json.loads((K/'FRONTEND_RESULT_r3.json').read_text());assert b['status']=='PASS'
assert b['source_sha256']==hashlib.sha256((K/'frontend-source-r3.tar.gz').read_bytes()).hexdigest()
for n,h in b['files'].items():assert hashlib.sha256((S/n).read_bytes()).hexdigest()==h
q=urllib.request.Request('http://127.0.0.1:8890/v1/chain/get_code_hash',json.dumps({'account_name':'eosio.key'}).encode(),{'Content-Type':'application/json'})
with urllib.request.urlopen(q,timeout=15)as r:assert json.load(r)['code_hash']=='1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d'
root=Path('/var/www/aic-test');current=root/'current';previous=str(current.resolve())
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-relay-execution-income-r1';release=root/'releases'/stamp
assert not release.exists();shutil.copytree(S,release)
for p in [release,*release.rglob('*')]:p.chmod(0o755 if p.is_dir()else 0o644)
for n,h in b['files'].items():assert hashlib.sha256((release/n).read_bytes()).hexdigest()==h
nextlink=root/('.next-'+stamp);nextlink.symlink_to(release);os.replace(nextlink,current)
result={'status':'PUBLISHED_AWAITING_PUBLIC_QA','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'previous_release':previous,'release':str(release),'source_commit':'097c1ad','source_sha256':b['source_sha256'],'files':b['files']}
(K/'FRONTEND_PUBLICATION.json').write_text(json.dumps(result,indent=2)+'\n')
print('REVIEWED_STATIC_FRONTEND_PUBLISHED '+stamp)
