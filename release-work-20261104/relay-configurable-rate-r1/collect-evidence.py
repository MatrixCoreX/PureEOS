"""Retrieve only public artifacts, reports and redacted logs; no chain databases."""
from pathlib import Path
import hashlib, importlib.util, json, subprocess

D = Path(__file__).resolve().parent
root = D.parents[1]
spec = importlib.util.spec_from_file_location('cloud', root/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)
owner = c.read()
assert owner['instance']['main_ip'] == '66.245.194.132'
code = r'''
from pathlib import Path
import hashlib,json,re,tarfile
K=Path('/opt/pureeos-key-rate-20261104-r1');F=Path('/opt/pureeos-explorer-key-rate-20261104-r3');C=Path('/opt/pureeos-explorer-key-rate-20261104-r4-collapse')
files=[]
for base,prefix in [(K,'contract'),(F,'frontend'),(C,'frontend-collapse')]:
 for pattern in ['*.json','*.log']:
  for p in base.glob(pattern):
   if p.name!='PUBLIC_EVIDENCE_MANIFEST.json':files.append((p,prefix+'/'+p.name))
for p in (K/'artifacts-r3').glob('eosio.key.*'):files.append((p,'artifacts/'+p.name))
for base,prefix in [(F,'frontend'),(C,'frontend-collapse')]:
 for p in base.glob('browser-*/*'):
  if p.suffix in ['.json','.png']:files.append((p,prefix+'/'+str(p.relative_to(base))))
for p in (K/'live-smoke').glob('*-RESULT.json'):files.append((p,'contract/live-smoke/'+p.name))
for n in ['r1','r3','r4','r5','r6']:
 p=Path('/opt/krate-q')/n/'RESULT.json'
 if p.exists():files.append((p,'private-qualification/'+n+'/RESULT.json'))
for p in Path('/opt/krate-q/r6').glob('local-tests/*/cpu-failures/*.log'):
 files.append((p,'private-qualification/r6/cpu-failures/'+p.name))
p=Path('/opt/krate-q/r6/candidate.abi.bin')
if p.exists():files.append((p,'artifacts/candidate.abi.bin'))
manifest={}
for p,name in files:
 data=p.read_bytes()
 assert not re.search(rb'PVT_[A-Z0-9]+_[A-Za-z0-9_-]{40,}',data),'Refuse private-key export in '+name
 assert not re.search(rb'(?<![1-9A-HJ-NP-Za-km-z])5[HJK][1-9A-HJ-NP-Za-km-z]{49}(?![1-9A-HJ-NP-Za-km-z])',data),'Refuse legacy private-key export in '+name
 manifest[name]=hashlib.sha256(data).hexdigest()
archive=K/'public-evidence.tar.gz'
with tarfile.open(archive,'w:gz')as tar:
 for p,name in files:tar.add(p,arcname=name,recursive=False)
(K/'PUBLIC_EVIDENCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'archive':str(archive),'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'files':manifest}))
'''
result = subprocess.run(c.ssh()+['python3','-B','-'],input=code,text=True,capture_output=True,check=True)
record = json.loads(result.stdout)
options = ['-i',owner['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+owner['known_hosts']]
archive = D/'public-evidence.tar.gz'
subprocess.run(['scp',*options,'root@66.245.194.132:'+record['archive'],str(archive)],check=True)
assert hashlib.sha256(archive.read_bytes()).hexdigest() == record['sha256']
(D/'PUBLIC_EVIDENCE_MANIFEST.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'files':len(record['files']),'sha256':record['sha256']}))
