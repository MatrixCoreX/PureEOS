from pathlib import Path
import json,subprocess,time,os,datetime,hashlib,urllib.request,re
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(n,d):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(d,indent=2)+'\n');os.replace(t,p)
def st(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,UnitFileState'],text=True).splitlines()if'='in l)
def rpc(n,d=None):
 q=urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+n,json.dumps(d or{}).encode(),{'Content-Type':'application/json'});return json.load(urllib.request.urlopen(q,timeout=8))
p=json.loads((R/'BANNED_STOP_PLAN.json').read_text());assert not(R/'BANNED_STOP_STARTED.json').exists();rows=rpc('get_table_rows',{'json':True,'code':'eosio','scope':'eosio','table':'rotcands','limit':1000});bans={v['owner']for v in rows['rows']if v['banned']};assert bans==set(p['banned'])and not rows['more']
protected={v['unit']:st(v['unit'])['MainPID']for name,v in p['all_known_nodes'].items()if name not in bans};protected['pureeos-devnet-history-20261105.service']=st('pureeos-devnet-history-20261105.service')['MainPID']
save('BANNED_STOP_STARTED.json',{'utc':now(),'protected_PIDs':protected,'banned':sorted(bans)})
result={}
for u in p['units_to_stop']:
 assert any(n in u for n in bans);before=st(u);subprocess.run(['systemctl','disable',u],capture_output=True,check=True,timeout=12)
 if before['MainPID']!='0' or before['ActiveState']=='failed':
  subprocess.run(['systemctl','stop','--no-block',u],capture_output=True,check=True,timeout=12);deadline=time.monotonic()+100
  while st(u)['MainPID']!='0'and time.monotonic()<deadline:time.sleep(1)
 after=st(u);assert after['MainPID']=='0'and after['UnitFileState']=='disabled',(u,after);result[u]={'before':before,'after':after};save('BANNED_STOP_PROGRESS.json',{'utc':now(),'stopped':result})
assert all(st(u)['MainPID']==pid for u,pid in protected.items())
# Extend reviewed config inventory to older retired native node trees only.
configs={x['config_dir']:x for x in p['exact_config_roots']};bases=[Path('/opt/pureeos-bp-demo-six-20261105-r1/nodes'),Path('/opt/pureeos-bp-add-two-stop-two-20261106-r1/nodes'),Path('/opt/pureeos-keynet-20261103-r1/bps'),Path('/opt/pureeos-keynet-20261103-r2/bps'),Path('/opt/pureeos-test45-20261102-r1/bps')]
for base in bases:
 for cfg in base.glob('*/config/config.ini'):
  if cfg.is_symlink():continue
  vals=re.findall(r'^\s*producer-name\s*=\s*([a-z1-5.]+)\s*$',cfg.read_text(),re.M)
  if len(vals)==1 and vals[0]in bans:configs[str(cfg.parent)]={'name':vals[0],'config_dir':str(cfg.parent),'data_dir':str(cfg.parent.parent/'data')}
private=[];targets={};safeties=[]
for conf,row in configs.items():
 c=Path(conf);assert c.resolve()==c and not c.is_symlink();cfg=c/'config.ini';assert cfg.is_file()
 raw=cfg.read_text();names=re.findall(r'^\s*producer-name\s*=\s*([a-z1-5.]+)\s*$',raw,re.M);assert names==[row['name']]and row['name']in bans
 # Store only signing/peer credentials, not a hidden full configuration copy.
 credential_lines=[l for l in raw.splitlines()if re.match(r'^\s*(signature-provider|peer-private-key)\s*=',l)];escrow=R/'private'/('credentials-'+hashlib.sha256(conf.encode()).hexdigest()[:16]+'.json');escrow.write_text(json.dumps({'owner':row['name'],'original_config':conf,'credential_lines':credential_lines}));escrow.chmod(0o600);private.append({'owner':row['name'],'source':conf,'credential_line_count':len(credential_lines),'escrow_bytes':escrow.stat().st_size})
 targets[conf]={'owner':row['name'],'kind':'configuration','path':conf,'delete_children_only':True,'config_sha256':hashlib.sha256(cfg.read_bytes()).hexdigest()}
 root=c.parent
 if root.name==row['name']:
  for data in root.glob('data*'):
   if data.is_dir()and not data.is_symlink():targets[str(data)]={'owner':row['name'],'kind':'data','path':str(data),'delete_children_only':True}
for path,row in targets.items():
 d=Path(path)
 if row['kind']=='data'and(d/'finalizers').exists():
  dest=R/'private'/('safety-'+hashlib.sha256(path.encode()).hexdigest()[:16]);dest.mkdir(mode=0o700)
  for f in(d/'finalizers').rglob('*'):
   if f.is_file():
    rel=f.relative_to(d/'finalizers');v=dest/rel;v.parent.mkdir(mode=0o700,parents=True,exist_ok=True);v.write_bytes(f.read_bytes());assert hashlib.sha256(f.read_bytes()).digest()==hashlib.sha256(v.read_bytes()).digest();safeties.append({'source':str(f),'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()})
 for f in d.rglob('*'):
  assert not f.is_symlink(),'Symlink in deletion root: '+str(f)
 row['allocated_bytes']=sum(f.stat().st_blocks*512 for f in d.rglob('*')if f.is_file());row['entries']=len(list(d.rglob('*')))
save('BANNED_DELETE_PLAN.json',{'status':'ALL_BANNED_STOPPED_EXACT_DELETE_READY','utc':now(),'stopped':result,'targets':list(targets.values()),'private_credential_escrow':private,'safety_preserved':safeties,'protected_PIDs':protected,'estimated_allocated_bytes':sum(v['allocated_bytes']for v in targets.values()),'scope':'User explicitly requests ALL banned blocks/config deletion. Independent server-only credentials/safety retained. Sharedgenesis/history/currenteligible/maintenance andother source/receipts untouched.'});save('BANNED_STOP_RESULT.json',{'status':'PASS_ALL21_IDENTITIES_STOPPED_DISABLED','utc':now(),'units':result,'protected_PIDs_unchanged':True});print(json.dumps({'status':'PASS_ALL_BANNED_STOPPED_DELETE_PLANNED','units':len(result),'targets':len(targets),'allocated_bytes':sum(v['allocated_bytes']for v in targets.values()),'safety_files':len(safeties)}))
