"""Collect evidence without keys/DBs. Frontend source stays in private storage.
The full archive must not be copied into the public contract repository.
"""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tarfile
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);owner=c.read();assert owner['instance']['main_ip']=='66.245.194.132'
remote=r'''
from pathlib import Path
import hashlib,json,re,tarfile
K=Path('/opt/pureeos-dev-proposals-20261104-r1');Q=Path('/opt/pureeos-key-ram-market-20261104-r1');files=[]
for p in K.iterdir():
 if p.is_file()and p.suffix in ['.json','.log','.ndjson','.py','.mjs','.cjs']:files.append((p,'reports/'+p.name))
for directory in sorted(K.glob('browser-*'))+sorted(K.glob('navigation-*')):
 if directory.is_dir():
  for p in directory.iterdir():
   if p.is_file()and p.suffix in ['.json','.png']:files.append((p,directory.name+'/'+p.name))
for p in sorted(K.glob('frontend-source-r[1-9].tar.gz')):files.append((p,'source/'+p.name))
files.append((K/'relay-rpc-split-source.tar.gz','source/relay-rpc-split-source.tar.gz'))
deployment=json.loads((K/'DEPLOYMENT.json').read_text());static=Path(deployment['release'])
for name,digest in deployment['files'].items():
 p=static/name;assert hashlib.sha256(p.read_bytes()).hexdigest()==digest;files.append((p,'static/'+name))
manifest=json.loads((K/'RUNTIME_PREPARED.json').read_text())
for name,digest in manifest['files'].items():
 p=K/'runtime'/name;assert hashlib.sha256(p.read_bytes()).hexdigest()==digest;files.append((p,'runtime/'+name))
split=json.loads((K/'RPC_SPLIT_TEST_RESULT.json').read_text())
for name,digest in split['files'].items():
 p=K/'runtime-rpcsplit'/name;assert hashlib.sha256(p.read_bytes()).hexdigest()==digest;files.append((p,'runtime-rpcsplit/'+name))
for name in ['eosio.key.wasm','eosio.key.abi','candidate.abi.bin']:files.append((Q/'artifacts'/name,'artifacts/'+name))
for unit in ['pureeos-keynet2-relay-20261103.service','pureeos-key-history-20261103.service']:
 for name in ['zz-ram-market-20261104.conf','zzz-relay-read-rpc-20261104.conf']:
  p=Path('/etc/systemd/system')/(unit+'.d')/name
  if p.exists():files.append((p,'unit-dropins/'+unit+'/'+name))
digests={}
for p,name in files:
 data=p.read_bytes()
 assert not re.search(rb'PVT_[A-Z0-9]+_[A-Za-z0-9_-]{40,}',data),'Private key in '+name
 assert not re.search(rb'(?<![1-9A-HJ-NP-Za-km-z])5[HJK][1-9A-HJ-NP-Za-km-z]{49}(?![1-9A-HJ-NP-Za-km-z])',data),'Legacy private key in '+name
 assert name not in digests;digests[name]=hashlib.sha256(data).hexdigest()
a=K/'verified-evidence.tar.gz'
with tarfile.open(a,'w:gz')as t:
 for p,name in files:t.add(p,arcname=name,recursive=False)
print(json.dumps({'archive':str(a),'sha256':hashlib.sha256(a.read_bytes()).hexdigest(),'files':digests}))
'''
q=subprocess.run(c.ssh()+['python3','-B','-'],input=remote,capture_output=True,text=True,check=True);record=json.loads(q.stdout)
opts=['-i',owner['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+owner['known_hosts']]
a=D/'verified-evidence.tar.gz';subprocess.run(['scp',*opts,'root@66.245.194.132:'+record['archive'],str(a)],check=True)
assert hashlib.sha256(a.read_bytes()).hexdigest()==record['sha256'];out=D/'evidence';out.mkdir(exist_ok=True)
with tarfile.open(a)as t:t.extractall(out,filter='data')
for name,digest in record['files'].items():assert hashlib.sha256((out/name).read_bytes()).hexdigest()==digest
(D/'VERIFIED_EVIDENCE_MANIFEST.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'files':len(record['files']),'archive_sha256':record['sha256'],'all_hashes_verified':True}))
