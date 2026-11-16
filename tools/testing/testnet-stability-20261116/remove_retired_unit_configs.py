from pathlib import Path
import subprocess,json,os,hashlib,shutil,re,datetime
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077);assert not(R/'RETIRED_UNIT_CONFIG_DELETE_STARTED.json').exists();plan=json.loads((R/'BANNED_STOP_PLAN.json').read_text());deleted=json.loads((R/'BANNED_DELETE_RESULT.json').read_text());assert deleted['status'].startswith('PASS_ALL21')
def st(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,StandardOutputFileToAppend,StandardErrorFileToAppend'],text=True).splitlines()if'='in l)
units=plan['units_to_stop'];assert all(st(u)['MainPID']=='0'for u in units);targets=[];logs=set()
for u in units:
 assert any(n in u for n in plan['banned'])
 for base in ['/etc/systemd/system','/run/systemd/system','/run/systemd/system.control']:
  root=Path(base)
  for p in [root/u,root/(u+'.d')]:
   if p.exists():assert not p.is_symlink();targets.append(p)
 for k,v in st(u).items():
  if k.endswith('FileToAppend')and v.startswith('/')and any(n in Path(v).parts for n in plan['banned']):logs.add(Path(v))
for row in deleted['targets']:
 p=Path(row['path']);owner=row['owner'];root=p.parent
 if p.name=='config'and root.name==owner:
  logs.update(root.glob('nodeos.log*'))
logs={p for p in logs if p.is_file()and not p.is_symlink()};manifest=[];secrets=set()
for p in targets:
 for f in ([p]if p.is_file()else list(p.rglob('*'))):
  if f.is_file():
   assert not f.is_symlink();raw=f.read_bytes();manifest.append({'path':str(f),'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw)});secrets.update(re.findall(r'PVT_[A-Z0-9]+_[A-Za-z0-9_-]+',raw.decode(errors='ignore')));secrets.update(re.findall(r'\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',raw.decode(errors='ignore')))
private=R/'private/retired-systemd-credentials.json';private.write_text(json.dumps({'private_values':sorted(secrets)}));private.chmod(0o600)
logmeta=[{'path':str(p),'bytes':p.stat().st_size,'allocated_bytes':p.stat().st_blocks*512,'inode':p.stat().st_ino}for p in logs];proof={'status':'STARTED','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'unit_files':manifest,'retired_logs':logmeta};(R/'RETIRED_UNIT_CONFIG_DELETE_STARTED.json').write_text(json.dumps(proof,indent=2))
# Reject open descriptors/maps before unlinking dedicated retired logs.
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:
  for fd in(proc/'fd').iterdir():
   try:assert Path(os.readlink(fd))not in logs,'Dedicated retired log still open'
   except OSError:pass
 except OSError:pass
for p in targets:
 if p.is_dir():shutil.rmtree(p)
 else:p.unlink()
for p in logs:p.unlink()
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True);assert all(st(u)['MainPID']=='0'for u in units)
proof.update(status='PASS_RETIRED_UNIT_CONFIG_AND_DEDICATED_LOGS_DELETED',utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),unit_file_count=len(manifest),dedicated_log_allocated_bytes=sum(x['allocated_bytes']for x in logmeta));(R/'RETIRED_UNIT_CONFIG_DELETE_RESULT.json').write_text(json.dumps(proof,indent=2));print(json.dumps(proof))
