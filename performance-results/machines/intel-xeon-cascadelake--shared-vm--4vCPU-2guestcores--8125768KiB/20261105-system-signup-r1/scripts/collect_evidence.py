"""Copy explicitly public reports/logs only; never copy fixture state or configs."""
from pathlib import Path
import base64, hashlib, json, re, shlex, subprocess
from remote import host, D

code = r'''
from pathlib import Path
import base64,json
r=Path('/opt/pureeos-system-signup-20261105-r1'); n=r/'native-r1'
paths=[r/x for x in ['WINDOW.json','RECOVERY.json','BEFORE_CHAIN.json','BEFORE_INVARIANTS.json','BEFORE_ACTIVITY.json','AFTER_CHAIN_BEFORE_ACTIVITY.json','POST_RESTORE_VERIFICATION.json','POST_RESTORE_HTTP.json','POST_RESTORE_ORIGIN.json','CLEANUP_RESULT.json','guardian.log','READ_RETRIES.jsonl']]
paths += [n/x for x in ['BUILD_RESULT.json','PATCHES.json','GENERATED_ADAPTATIONS.json','JOBS.json']]
paths += [r/'HARDWARE.json',r/'QUALIFICATION_SOURCE_HASHES.json']
paths += [r/'RESTORE_SERVICE_RETRY.json'] + list((r/'origin-attempts').glob('*.json'))
paths += list(r.glob('native-build-r*.log')) + list(r.glob('qualify-r*.log'))
paths += list(n.glob('BUILD_FAILURE_*.json')) + list(n.glob('BUILD_INTERRUPTED_*.json'))
for d in sorted(n.glob('qualification-*')):
 paths += [p for p in d.iterdir() if p.is_file() and p.suffix in ['.json','.xml','.log']]
paths += [n/'real-contracts/eosio.signup.raw.abi']
out={str(p.relative_to(r)):base64.b64encode(p.read_bytes()).decode() for p in paths if p.exists()}
print(json.dumps(out))
'''
raw=subprocess.run(host.ssh()+['python3','-c',shlex.quote(code)],text=True,capture_output=True,check=True)
files=json.loads(raw.stdout); root=D/'evidence/maintenance-native'; manifest={}
for name, encoded in files.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    data=base64.b64decode(encoded)
    assert not re.search(rb'PVT_[A-Z0-9]+_[1-9A-HJ-NP-Za-km-z]+',data), 'Refuse private key output'
    assert not re.search(rb'\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',data), 'Refuse WIF-like output'
    path=root/name; path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    manifest[name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
(root/'MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'public_files':len(manifest),'bytes':sum(v['bytes'] for v in manifest.values())}))
