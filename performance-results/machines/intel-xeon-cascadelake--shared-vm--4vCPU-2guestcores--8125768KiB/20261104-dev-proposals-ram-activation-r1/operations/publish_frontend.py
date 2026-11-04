"""Publish the verified static files atomically, preserving the previous release."""
from pathlib import Path
import datetime,hashlib,json,os,shutil,subprocess,sys
K=Path('/opt/pureeos-dev-proposals-20261104-r1');S=K/'frontend/dist';site=Path('/var/www/aic-test')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
proof=sys.argv[1]if len(sys.argv)>1 else'browser-preview'
assert proof in ['browser-preview','navigation-preview']
result=json.loads((K/'FRONTEND_RESULT.json').read_text());preview=json.loads((K/proof/'RESULT.json').read_text())
assert result['status']==preview['status']=='PASS'and result['files']==preview['build_files']
assert not(K/'DEPLOYMENT.json').exists()
assert result['files']=={str(p.relative_to(S)):sha(p)for p in S.rglob('*')if p.is_file()}
intent=K/'PUBLISH_INTENT.json'
if intent.exists():
 pending=json.loads(intent.read_text());old=Path(pending['previous_release']);release=Path(pending['release'])
 assert(site/'current').resolve()==release
else:
 old=(site/'current').resolve();release=site/'releases'/(datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-dev-proposals-ram-fees')
 intent.write_text(json.dumps({'previous_release':str(old),'release':str(release)},indent=2)+'\n')
 shutil.copytree(S,release)
 temporary=site/'current-dev-proposals-next';assert not temporary.exists();temporary.symlink_to(release);os.replace(temporary,site/'current')
old_files={str(p.relative_to(old)):sha(p)for p in old.rglob('*')if p.is_file()}
assert result['files']=={str(p.relative_to(release)):sha(p)for p in release.rglob('*')if p.is_file()}
# Match the browser/previous release verifier's HTTP client; Python urllib is
# denied by this public origin even though Node and Chromium return the files.
verifier="""const fs=require('node:fs'),crypto=require('node:crypto');(async()=>{const files=JSON.parse(fs.readFileSync(0,'utf8'));for(const [name,hash]of Object.entries(files)){const r=await fetch('https://aic-test.matrixai.one/'+name+'?dev-proposals='+Date.now(),{signal:AbortSignal.timeout(40000)});if(!r.ok||crypto.createHash('sha256').update(Buffer.from(await r.arrayBuffer())).digest('hex')!==hash)throw Error('Public digest mismatch: '+name+' HTTP '+r.status);}console.log(JSON.stringify(files));})().catch(e=>{console.error(e);process.exitCode=1;});"""
verified=subprocess.run(['/opt/pureeos-keynet-20261103-r2/tools/node-current/bin/node','-e',verifier],input=json.dumps(result['files']),text=True,capture_output=True,check=True)
public=json.loads(verified.stdout)
record={'status':'PUBLISHED','release':str(release),'previous_release':str(old),'files':result['files'],'public_verified':public,
 'previous_files':old_files,'source_sha256':result['source_sha256'],'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
 'read_only':True,'nginx_changed':False,'chain_changed':False}
(K/'DEPLOYMENT.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
