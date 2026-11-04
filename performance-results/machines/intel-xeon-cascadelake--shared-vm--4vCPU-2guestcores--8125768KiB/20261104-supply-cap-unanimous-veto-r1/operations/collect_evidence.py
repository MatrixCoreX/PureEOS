"""Collect public governance evidence and candidate sources; never private signing material."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tarfile
D=Path(__file__).resolve().parent;W=D.parents[1];sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py');c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);s=c.read()
remote=r'''
from pathlib import Path
import hashlib,json,re,tarfile
K=Path('/opt/pureeos-cap-veto-20261104-r1');files=[]
for p in K.iterdir():
 if p.is_file()and(p.suffix in ['.json','.log','.py']or p.name.startswith('candidate-source-')and p.name.endswith('.tar.gz')):files.append((p,'reports/'+p.name))
for folder in ['vetoes','artifacts','artifacts-failed-r1','source-failed-r1']:
 for p in(K/folder).rglob('*'):
  if p.is_file():files.append((p,folder+'/'+str(p.relative_to(K/folder))))
manifest={}
for p,name in files:
 data=p.read_bytes();assert not re.search(rb'PVT_[A-Z0-9]+_[A-Za-z0-9_-]{40,}',data),name
 assert not re.search(rb'(?<![1-9A-HJ-NP-Za-km-z])5[HJK][1-9A-HJ-NP-Za-km-z]{49}(?![1-9A-HJ-NP-Za-km-z])',data),name
 manifest[name]=hashlib.sha256(data).hexdigest()
a=K/'verified-public-evidence.tar.gz'
with tarfile.open(a,'w:gz')as t:
 for p,name in files:t.add(p,arcname=name,recursive=False)
print(json.dumps({'archive':str(a),'sha256':hashlib.sha256(a.read_bytes()).hexdigest(),'files':manifest}))
'''
p=subprocess.run(c.ssh()+['python3','-B','-'],input=remote,capture_output=True,text=True,check=True);r=json.loads(p.stdout)
o=['-i',s['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+s['known_hosts']]
a=D/'verified-public-evidence.tar.gz';subprocess.run(['scp',*o,'root@66.245.194.132:'+r['archive'],str(a)],check=True)
assert hashlib.sha256(a.read_bytes()).hexdigest()==r['sha256'];E=D/'evidence';E.mkdir(exist_ok=True)
with tarfile.open(a)as t:t.extractall(E,filter='data')
for name,digest in r['files'].items():assert hashlib.sha256((E/name).read_bytes()).hexdigest()==digest
(D/'VERIFIED_EVIDENCE_MANIFEST.json').write_text(json.dumps(r,indent=2)+'\n');print('Verified public cap evidence:',len(r['files']))
