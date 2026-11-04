"""Remove only this task's inactive frontend work copy after verified evidence."""
from pathlib import Path
import datetime,hashlib,json,os,re,shutil,subprocess,sys,tarfile
K=Path('/opt/pureeos-dev-proposals-20261104-r1');digest=sys.argv[1];assert re.fullmatch('[0-9a-f]{64}',digest)
assert not(K/'CLEANUP_RESULT.json').exists()
for name,expected in [('EXECUTION.json','UPGRADED_IRREVERSIBLE'),('APPS_SWITCHED.json','PASS'),('LIVE_SMOKE.json','PASS'),('API_after_PUBLIC_RESULT.json','PASS'),
 ('LIVE_FEE_INTEGRATION.json','PASS'),('browser-public-after/RESULT.json','PASS'),('navigation-public/RESULT.json','PASS'),('LIVE_AUDIT_after.json','PASS'),('RPC_SPLIT_TEST_RESULT.json','PASS'),('RPC_SPLIT_SWITCHED.json','PASS')]:
 assert json.loads((K/name).read_text())['status']==expected,name
archive=K/'verified-evidence.tar.gz';assert hashlib.sha256(archive.read_bytes()).hexdigest()==digest
with tarfile.open(archive)as t:
 assert hashlib.sha256(t.extractfile('source/frontend-source-r4.tar.gz').read()).hexdigest()=='80ad955b35218680568deb4627785b452bd14212118cc1cf84cfab521f76a99a'
 assert hashlib.sha256(t.extractfile('artifacts/eosio.key.wasm').read()).hexdigest()=='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
 assert json.loads(t.extractfile('reports/LIVE_SMOKE.json').read())['status']=='PASS'
paths=[K/'frontend',K/'styles.css'];assert all(p.exists()for p in paths)
prefix=str(K/'frontend')
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:
  assert prefix not in os.readlink(proc/'cwd'),'Frontend working directory still active'
  assert prefix.encode()not in(proc/'cmdline').read_bytes(),'Frontend process still active'
  for fd in(proc/'fd').iterdir():
   try:assert not os.readlink(fd).startswith(prefix+'/'),'Frontend file still open'
   except FileNotFoundError:pass
 except(ProcessLookupError,FileNotFoundError,PermissionError):continue
dependency=K/'runtime/node_modules';target=dependency.resolve();identity=(target.stat().st_dev,target.stat().st_ino);assert target.is_dir()
assert(K/'runtime-rpcsplit/node_modules').resolve()==target
old=json.loads(Path('/opt/pureeos-key-rate-20261104-r1/LIVE_AUDIT_after.json').read_text())
pids={u:int(subprocess.check_output(['systemctl','show',u,'-p','MainPID','--value'],text=True))for u in old['node_pids']};assert pids==old['node_pids']
allocated=0;seen=set()
def count(p):
 global allocated
 s=p.lstat();key=(s.st_dev,s.st_ino)
 if key not in seen:allocated+=s.st_blocks*512;seen.add(key)
for path in paths:
 if path.is_dir():
  for root,dirs,files in os.walk(path,followlinks=False):
   count(Path(root))
   for n in files:count(Path(root)/n)
   for n in dirs:
    p=Path(root)/n
    if p.is_symlink():count(p)
 else:count(path)
before=os.statvfs('/');plan={'paths':[str(p)for p in paths],'allocated_bytes':allocated,'verified_evidence_backup_sha256':digest,'shared_dependency_target':str(target),
 'current_static_runtime_keys_chain_history_preserved':True,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()}
(K/'CLEANUP_PLAN.json').write_text(json.dumps(plan,indent=2)+'\n')
for p in paths:
 if p.is_dir()and not p.is_symlink():shutil.rmtree(p)
 else:p.unlink()
assert target.is_dir()and(target.stat().st_dev,target.stat().st_ino)==identity
assert all(int(subprocess.check_output(['systemctl','show',u,'-p','MainPID','--value'],text=True))==pid for u,pid in pids.items())
after=os.statvfs('/');result={**plan,'status':'PASS','allocated_removed_MiB':round(allocated/1048576,3),'filesystem_net_free_delta_MiB':round((after.f_bfree-before.f_bfree)*after.f_frsize/1048576,3),
 'net_delta_includes_ongoing_chain_activity':True,'available_GiB':round(after.f_bavail*after.f_frsize/1073741824,3),'original_26_PIDs_preserved':True,'active_runtime_and_shared_dependencies_preserved':True}
(K/'CLEANUP_RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
