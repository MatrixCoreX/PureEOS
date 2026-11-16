from pathlib import Path
import json,subprocess,datetime,os,hashlib,shutil,re
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(n,d):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(d,indent=2)+'\n');os.replace(t,p)
def st(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,UnitFileState'],text=True).splitlines()if'='in l)
p=json.loads((R/'BANNED_DELETE_PLAN.json').read_text());assert not(R/'BANNED_DELETE_STARTED.json').exists();assert p['status']=='ALL_BANNED_STOPPED_EXACT_DELETE_READY';assert all(st(u)['MainPID']=='0'and st(u)['UnitFileState']=='disabled'for u in p['stopped'])
roots=[Path(x['path'])for x in p['targets']];assert all(x.resolve()==x and x.is_dir()and not x.is_symlink()for x in roots)
refs=[]
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 paths=[]
 try:
  for line in(proc/'maps').read_text().splitlines():
   pieces=line.split(None,5)
   if len(pieces)==6:paths.append(pieces[5])
 except OSError:pass
 try:
  for fd in(proc/'fd').iterdir():
   try:paths.append(os.readlink(fd))
   except OSError:pass
 except OSError:pass
 for attr in ['cwd','exe']:
  try:paths.append(os.readlink(proc/attr))
  except OSError:pass
 hits=[v for v in paths if v.startswith('/')and any(Path(v)==r or r in Path(v).parents for r in roots)]
 if hits:refs.append({'PID':proc.name,'references':hits[:3]})
assert not refs,refs
for row in p['targets']:
 d=Path(row['path']);assert row['owner']in json.loads((R/'BANNED_STOP_PLAN.json').read_text())['banned'];assert all(not f.is_symlink()for f in d.rglob('*'))
 if row['kind']=='configuration':
  assert hashlib.sha256((d/'config.ini').read_bytes()).hexdigest()==row['config_sha256'];secretvalues=set()
  for f in d.rglob('*'):
   if f.is_file()and f.stat().st_size<1024**2:
    raw=f.read_text(errors='ignore');secretvalues.update(re.findall(r'PVT_[A-Z0-9]+_[A-Za-z0-9_-]+',raw));secretvalues.update(re.findall(r'\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',raw))
  escrow=R/'private'/('remaining-secretvalues-'+hashlib.sha256(str(d).encode()).hexdigest()[:16]+'.json');escrow.write_text(json.dumps({'owner':row['owner'],'values':sorted(secretvalues)}));escrow.chmod(0o600)
assert all(st(u)['MainPID']==pid for u,pid in p['protected_PIDs'].items())
before=os.statvfs('/');save('BANNED_DELETE_STARTED.json',{'utc':now(),'targets':p['targets']});done=[]
for row in p['targets']:
 d=Path(row['path']);deleted=0
 for child in list(d.iterdir()):
  if row['kind']=='data'and child.name=='finalizers':continue
  assert not child.is_symlink()and not os.path.ismount(child)
  if child.is_dir():shutil.rmtree(child)
  else:child.unlink()
  deleted+=1
 done.append({'owner':row['owner'],'path':str(d),'kind':row['kind'],'previous_allocated_bytes':row['allocated_bytes'],'deleted_direct_entries':deleted,'remaining':[x.name for x in d.iterdir()]});save('BANNED_DELETE_PROGRESS.json',{'utc':now(),'completed':done})
for s in p['safety_preserved']:assert hashlib.sha256(Path(s['source']).read_bytes()).hexdigest()==s['sha256']
assert all(st(u)['MainPID']==pid for u,pid in p['protected_PIDs'].items());after=os.statvfs('/');result={'status':'PASS_ALL21_BANNED_IDENTITIES_STOPPED_BLOCKS_CONFIGS_DELETED','utc':now(),'units_stopped_disabled':len(p['stopped']),'targets':done,'independent_server_private_credentials_and_safety_retained':True,'protected_PIDs_unchanged':True,'filesystem_available_delta_bytes':(after.f_bavail-before.f_bavail)*after.f_frsize};save('BANNED_DELETE_RESULT.json',result);print(json.dumps(result))
