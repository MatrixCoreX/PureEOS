"""Apply the reviewed exact seven-root plan; retain configuration, keys and every safety file."""
import datetime,gzip,hashlib,json,os,shutil,stat,subprocess
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1');os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(n,x):
    p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(x,ensure_ascii=False,indent=2)+'\n');os.replace(t,p)
def state(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,UnitFileState'],text=True).splitlines()if'='in l)
def digest(f):
    h=hashlib.sha256()
    for b in iter(lambda:f.read(1024**2),b''):h.update(b)
    return h.hexdigest()
plan=json.loads((R/'CLEANUP_PLAN.json').read_text());pre=json.loads((R/'STOP_STARTED.json').read_text())
assert plan['status']=='STOPPED_AND_EXACT_CLEANUP_PLANNED'
assert not(R/'CLEANUP_STARTED.json').exists(),'Reconcile receipts; never rerun deletion'
roots=[Path(p['root'])for p in plan['plans']if'root'in p]
assert len(roots)==7
assert all(state(p['unit'])['MainPID']=='0' and state(p['unit'])['UnitFileState']=='disabled'for p in plan['plans'])
assert all(state(u)['MainPID']==pid for u,pid in pre['protected_PIDs'].items())
refs=[]
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():continue
    paths=[]
    try:
        for line in(proc/'maps').read_text().splitlines():
            v=line.split(None,5)
            if len(v)==6:paths.append(v[5])
    except OSError:pass
    try:
        for fd in(proc/'fd').iterdir():
            try:paths.append(os.readlink(fd))
            except OSError:pass
    except OSError:pass
    for attr in ['cwd','exe']:
        try:paths.append(os.readlink(proc/attr))
        except OSError:pass
    hits=[p for p in paths if p.startswith('/')and any(Path(p)==r or r in Path(p).parents for r in roots)]
    if hits:refs.append({'pid':proc.name,'references':hits[:4]})
assert not refs,refs
for p in plan['plans']:
    for e in p.get('entries',[]):
        f=Path(e['path']);t=f.lstat()
        assert(t.st_dev,t.st_ino,t.st_size,t.st_mtime_ns,t.st_mode)==(e['dev'],e['ino'],e['size'],e['mtime_ns'],e['mode']),str(f)
        assert not f.is_symlink()
before=os.statvfs('/');save('CLEANUP_STARTED.json',{'utc':now(),'data_allocated_bytes':plan['data_allocated_bytes'],'roots':[str(p)for p in roots]})
completed=[]
for p in plan['plans']:
    assert state(p['unit'])['MainPID']=='0'
    if'root'in p:
        root=Path(p['root']);assert root.resolve(strict=True)==root
        if(root/'finalizers').exists():
            dest=R/'retained-finalizer-safety'/p['owner']/root.name
            dest.parent.mkdir(parents=True,exist_ok=True,mode=0o700);shutil.copytree(root/'finalizers',dest)
        assert shutil.rmtree.avoids_symlink_attacks
        for child in root.iterdir():
            if child.name=='finalizers':continue
            assert child.name in ['blocks','state','snapshots','state.before-wallet-build-20261106','state.snapshot-before-reversible-refresh-20261106'],str(child)
            assert child.is_dir()and not child.is_symlink()and not os.path.ismount(child)
            shutil.rmtree(child)
        for e in p['retained_safety']:
            with Path(e['path']).open('rb')as f:assert digest(f)==e['sha256']
        assert all(f.name=='finalizers'for f in root.iterdir())
        completed.append({'owner':p['owner'],'root':str(root),'removed_allocated_bytes':p['remove_allocated_bytes'],'retained_safety':p['retained_safety']})
    elif p['bytes']:
        src=Path(p['log']);dest=R/(p['owner']+'-original-nodeos.log.gz');assert not dest.exists()
        with src.open('rb')as f:original=digest(f)
        with src.open('rb')as f,dest.open('xb')as out:
            q=subprocess.run(['nice','-n','19','gzip','-1','-c'],stdin=f,stdout=out,stderr=subprocess.PIPE,timeout=150);assert q.returncode==0
        with gzip.open(dest,'rb')as f:assert digest(f)==original
        assert src.stat().st_size==p['bytes'];src.unlink()
        completed.append({'owner':p['owner'],'log':str(src),'preserved_gzip':str(dest),'uncompressed_sha256':original,'old_allocated':p['allocated'],'new_allocated':dest.stat().st_blocks*512})
    save('CLEANUP_PROGRESS.json',{'utc':now(),'completed':completed})
assert all(state(u)['MainPID']==pid for u,pid in pre['protected_PIDs'].items())
after=os.statvfs('/')
result={'status':'PASS_FOUR_NEW_BANS_STOPPED_SEVEN_DATA_ROOTS_CLEARED','utc':now(),'completed':completed,'removed_data_allocated_bytes':plan['data_allocated_bytes'],'verified_log_savings_bytes':sum(x['old_allocated']-x['new_allocated']for x in completed if'old_allocated'in x),'filesystem_available_delta_bytes':(after.f_bavail-before.f_bavail)*after.f_frsize,'protected_PIDs_unchanged':True,'old11_cleanup_not_replayed':True,'all_configuration_keys_finalizer_safety_retained':True}
save('CLEANUP_RESULT.json',result);print(json.dumps(result,ensure_ascii=False))
