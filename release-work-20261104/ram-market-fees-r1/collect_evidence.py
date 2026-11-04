"""Retrieve allowlisted public reports/artifacts, never keys or chain databases."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tarfile
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);s=c.read();assert s['instance']['main_ip']=='66.245.194.132'
remote=r'''
from pathlib import Path
import hashlib,json,re,tarfile
K=Path('/opt/pureeos-key-ram-market-20261104-r1');files=[]
for p in K.iterdir():
 if p.is_file()and p.suffix in ['.json','.log']:files.append((p,'reports/'+p.name))
for p in (K/'artifacts').glob('eosio.key.*'):files.append((p,'artifacts/'+p.name))
for p in (K/'browser').glob('*'):
 if p.suffix in ['.json','.png']:files.append((p,'browser/'+p.name))
for label in ['native','fixture','native-r2','fixture-r2','native-r3','fixture-r3']:
 work=Path('/opt/kram-q')/label;p=work/'RESULT.json'
 if p.exists():files.append((p,'qualification/'+label+'/RESULT.json'))
 for p in work.glob('local-tests/*/cpu-failures/*.log'):files.append((p,'qualification/'+label+'/cpu-failures/'+p.name))
 if label=='native-r3':
  for p in work.glob('local-tests/*/candidate.abi.bin'):files.append((p,'artifacts/candidate.abi.bin'))
manifest={}
for p,name in files:
 data=p.read_bytes()
 assert not re.search(rb'PVT_[A-Z0-9]+_[A-Za-z0-9_-]{40,}',data),'Private key in '+name
 assert not re.search(rb'(?<![1-9A-HJ-NP-Za-km-z])5[HJK][1-9A-HJ-NP-Za-km-z]{49}(?![1-9A-HJ-NP-Za-km-z])',data),'Legacy private key in '+name
 manifest[name]=hashlib.sha256(data).hexdigest()
a=K/'public-evidence.tar.gz'
with tarfile.open(a,'w:gz')as t:
 for p,name in files:t.add(p,arcname=name,recursive=False)
print(json.dumps({'archive':str(a),'sha256':hashlib.sha256(a.read_bytes()).hexdigest(),'files':manifest}))
'''
q=subprocess.run(c.ssh()+['python3','-B','-'],input=remote,text=True,capture_output=True,check=True)
record=json.loads(q.stdout);opts=['-i',s['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+s['known_hosts']]
a=D/'public-evidence.tar.gz';subprocess.run(['scp',*opts,'root@66.245.194.132:'+record['archive'],str(a)],check=True)
assert hashlib.sha256(a.read_bytes()).hexdigest()==record['sha256']
out=D/'evidence';out.mkdir(exist_ok=True)
with tarfile.open(a)as t:t.extractall(out,filter='data')
for name,h in record['files'].items():assert hashlib.sha256((out/name).read_bytes()).hexdigest()==h
(D/'PUBLIC_EVIDENCE_MANIFEST.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'files':len(record['files']),'archive_sha256':record['sha256'],'all_hashes_verified':True}))
