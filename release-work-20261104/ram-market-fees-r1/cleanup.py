"""User-requested cleanup: back up public evidence, verify locally, then delete
only stopped private test data and this task's duplicate/cached work files.
Never touches current keynet-r2, history databases, active relay or toolchains.
"""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tarfile
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);owner=c.read()
assert owner['instance']['main_ip']=='66.245.194.132'
prepare=r'''
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,tarfile
K=Path('/opt/pureeos-key-ram-market-20261104-r1')
assert json.loads((K/'APPLICATION_REVIEW_r5.json').read_text())['status']=='PASS'
for label in ['native','fixture']:assert json.loads((K/('QUALIFICATION_'+label+'_r3.json')).read_text())['status']=='PASS'
for unit in ['pureeos-key-ram-qualify-r3-20261104','pureeos-key-ram-browser-r5-20261104']:
 assert subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True).strip()=='0',unit
assert hashlib.sha256((K/'artifacts/eosio.key.wasm').read_bytes()).hexdigest()=='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
def safe(data,name):
 assert not re.search(rb'PVT_[A-Z0-9]+_[A-Za-z0-9_-]{40,}',data),'Private key in '+name
 assert not re.search(rb'(?<![1-9A-HJ-NP-Za-km-z])5[HJK][1-9A-HJ-NP-Za-km-z]{49}(?![1-9A-HJ-NP-Za-km-z])',data),'Legacy private key in '+name
def disk():
 s=os.statvfs('/');return {'total':s.f_blocks*s.f_frsize,'used':(s.f_blocks-s.f_bfree)*s.f_frsize,'available':s.f_bavail*s.f_frsize}
source=K/'source-r3';kit=K/'source-qualified.tar.gz';source_files={}
with tarfile.open(kit,'w:gz')as t:
 for p in sorted(source.rglob('*')):
  if p.is_file()and not set(p.relative_to(source).parts)&{'node_modules','__pycache__'}:
   name=str(p.relative_to(source));data=p.read_bytes();safe(data,name);source_files[name]=hashlib.sha256(data).hexdigest();t.add(p,arcname=name,recursive=False)
(K/'QUALIFIED_SOURCE_MANIFEST.json').write_text(json.dumps({'archive_sha256':hashlib.sha256(kit.read_bytes()).hexdigest(),'files':source_files},indent=2)+'\n')
roots=[Path('/opt/kram-q'),Path('/opt/krate-q'),Path('/opt/kclaim-q')];evidence=[];remove=[]
for root in roots:
 if not root.exists():continue
 for work in sorted(root.iterdir()):
  if not work.is_dir()or work.is_symlink()or not(work/'RESULT.json').exists():continue
  evidence.append((work/'RESULT.json',root.name+'/'+work.name+'/RESULT.json'))
  for p in work.glob('local-tests/*/cpu-failures/*.log'):evidence.append((p,root.name+'/'+work.name+'/cpu-failures/'+p.name))
  for name in ['local-tests','programs']:
   p=work/name
   if p.exists():remove.append(p)
backup=K/'cleanup-private-test-reports.tar.gz';files={}
with tarfile.open(backup,'w:gz')as t:
 for p,name in evidence:
  data=p.read_bytes();safe(data,name);files[name]=hashlib.sha256(data).hexdigest();t.add(p,arcname=name,recursive=False)
for p in Path('/opt/kram-q/native-r3').glob('local-tests/*/candidate.abi.bin'):shutil.copyfile(p,K/'artifacts/candidate.abi.bin')
for name in ['source','source-final','source-r2','source-r3/node_modules','source-r3/sdk-tests/__pycache__','source-r3/tests/__pycache__','frontend/node_modules','frontend/remote-integration-build','frontend/.vite']:
 p=K/name
 if p.exists()or p.is_symlink():remove.append(p)
items=[]
for p in remove:
 assert any(p.is_relative_to(root)for root in [K,*roots])
 size=int(subprocess.check_output(['du','-s','-B1',str(p)],text=True).split()[0])
 items.append({'path':str(p),'kind':'symlink'if p.is_symlink()else'directory','allocated_bytes':size})
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:cmd=(proc/'cmdline').read_bytes()
 except(OSError,PermissionError):continue
 assert not any(i['kind']!='symlink'and i['path'].encode()in cmd for i in items),'Private path still in use by PID '+proc.name
plan={'scope':'User requested cleanup; only stopped private test data and this task duplicate sources/caches','before':disk(),'items':items,
 'backup_sha256':hashlib.sha256(backup.read_bytes()).hexdigest(),'backup_files':files,'source_kit_sha256':hashlib.sha256(kit.read_bytes()).hexdigest(),
 'preserve':['current keynet-r2 and its keys/services','all active history databases','sole founder relay source-r3 in the earlier rate task','qualified toolchain/browser/shared dependency caches','artifacts/reports/failures/final source kit and current frontend dist']}
(K/'CLEANUP_PLAN.json').write_text(json.dumps(plan,indent=2)+'\n');print(json.dumps(plan))
'''
q=subprocess.run(c.ssh()+['python3','-B','-'],input=prepare,text=True,capture_output=True,check=True)
plan=json.loads(q.stdout);(D/'CLEANUP_PLAN.json').write_text(json.dumps(plan,indent=2)+'\n')
opts=['-i',owner['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+owner['known_hosts']]
for name,h in [('cleanup-private-test-reports.tar.gz',plan['backup_sha256']),('source-qualified.tar.gz',plan['source_kit_sha256'])]:
 p=D/name;subprocess.run(['scp',*opts,'root@66.245.194.132:/opt/pureeos-key-ram-market-20261104-r1/'+name,str(p)],check=True);assert hashlib.sha256(p.read_bytes()).hexdigest()==h
with tarfile.open(D/'cleanup-private-test-reports.tar.gz')as t:
 for name,h in plan['backup_files'].items():assert hashlib.sha256(t.extractfile(name).read()).hexdigest()==h
apply=r'''
from pathlib import Path
import datetime,hashlib,json,os,shutil,subprocess
K=Path('/opt/pureeos-key-ram-market-20261104-r1');plan=json.loads((K/'CLEANUP_PLAN.json').read_text());assert plan['backup_sha256']==BACKUP
assert hashlib.sha256((K/'cleanup-private-test-reports.tar.gz').read_bytes()).hexdigest()==BACKUP
assert hashlib.sha256((K/'source-qualified.tar.gz').read_bytes()).hexdigest()==SOURCE
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:cmd=(proc/'cmdline').read_bytes()
 except(OSError,PermissionError):continue
 assert not any(i['kind']!='symlink'and i['path'].encode()in cmd for i in plan['items']),'Path still active PID '+proc.name
removed=[]
for item in plan['items']:
 p=Path(item['path']);assert any(p.is_relative_to(root)for root in [K,Path('/opt/kram-q'),Path('/opt/krate-q'),Path('/opt/kclaim-q')])
 if item['kind']=='symlink':assert p.is_symlink();p.unlink()
 else:assert p.is_dir()and not p.is_symlink();shutil.rmtree(p)
 assert not p.exists()and not p.is_symlink();removed.append(item)
s=os.statvfs('/');after={'total':s.f_blocks*s.f_frsize,'used':(s.f_blocks-s.f_bfree)*s.f_frsize,'available':s.f_bavail*s.f_frsize}
result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'before':plan['before'],'after':after,'removed':removed,
 'removed_allocated_bytes':sum(i['allocated_bytes']for i in removed),'filesystem_used_delta_bytes':plan['before']['used']-after['used'],
 'backup_sha256':BACKUP,'source_kit_sha256':SOURCE,'local_backups_hash_verified_before_deletion':True,'active_service_paths_deleted':False,'preserved':plan['preserve']}
(K/'CLEANUP_RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
'''.replace('BACKUP',repr(plan['backup_sha256'])).replace('SOURCE',repr(plan['source_kit_sha256']))
q=subprocess.run(c.ssh()+['python3','-B','-'],input=apply,text=True,capture_output=True,check=True)
(D/'CLEANUP_RESULT.json').write_text(q.stdout);result=json.loads(q.stdout)
print(json.dumps({'status':result['status'],'removed_allocated_MiB':round(result['removed_allocated_bytes']/1048576,2),'available_GiB':round(result['after']['available']/1073741824,2),'paths':len(result['removed'])}))
