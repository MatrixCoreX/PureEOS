"""Copy allowlisted public evidence only, with a private-key rejection gate."""
from pathlib import Path
import base64,hashlib,json,re,shlex,subprocess,sys
D=Path(__file__).resolve().parent
sys.path.insert(0,str(D.parent/'system-signup-r1'))
from remote import host
code=r'''
from pathlib import Path
import base64,json
r=Path('/opt/pureeos-system-signup-fix-20261105-r2')
names=['APPROVAL.json','WINDOW.json','RECOVERY.json','SOURCE_HASHES.json','LIVE_BASELINE.json',
 'BEFORE_CHAIN.json','BEFORE_INVARIANTS.json','BEFORE_ACTIVITY.json','AFTER_CHAIN_BEFORE_ACTIVITY.json',
 'POST_RESTORE_VERIFICATION.json','POST_RESTORE_ORIGIN.json','POST_RESTORE_PROGRESS.json','DATABASE_READY.json','RESOURCE_FAILURE.json',
 'BUILD_RESOURCE_RECORD.json','BUILD3_RESOURCE_RECORD.json','WIRE_HELPER_STAGING_FIX.json',
 'CLEANUP_RESULT.json','NODE_VERSION_SMOKE.json','guardian.log','READ_RETRIES.jsonl','pause.log','restore-audit.log','progress-audit.log']
paths=[r/n for n in names]+list(r.glob('build-*.log'))+list(r.glob('test-*.log'))+list((r/'origin-attempts').glob('*.json'))
for d in sorted(r.glob('build-*')):
 if d.is_dir():paths += [p for p in d.iterdir() if p.is_file() and p.suffix in ['.json','.log']]
for d in sorted(r.glob('qualification-*')):
 paths += [p for p in d.iterdir() if p.is_file() and p.suffix in ['.json','.xml','.log']]
paths += [r/'real-contracts/eosio.signup.raw.abi']
out={str(p.relative_to(r)):base64.b64encode(p.read_bytes()).decode() for p in paths if p.exists()}
old=Path('/opt/pureeos-system-signup-20261105-r1')
for n in ['HARDWARE.json','native-r1/INPUTS.json','native-r1/PATCHES.json','native-r1/JOBS.json']:
 p=old/n
 if p.exists():out['retained-base/'+n]=base64.b64encode(p.read_bytes()).decode()
print(json.dumps(out))
'''
result=subprocess.run(host.ssh()+['python3','-c',shlex.quote(code)],capture_output=True,text=True)
if result.returncode:
 print(result.stderr[-2000:]);raise SystemExit(result.returncode)
files=json.loads(result.stdout);manifest={};root=D/'evidence'
for name,encoded in files.items():
 assert not Path(name).is_absolute() and '..' not in Path(name).parts
 data=base64.b64decode(encoded)
 assert not re.search(rb'PVT_[A-Z0-9]+_[1-9A-HJ-NP-Za-km-z]+',data),'Refuse private-key output'
 assert not re.search(rb'\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',data),'Refuse WIF-like output'
 p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
 manifest[name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
(root/'MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'public_files':len(manifest),'bytes':sum(v['bytes'] for v in manifest.values())}))
