"""Hot-reload BP warning logs; qualify one quorum-guarded same-data runtime recycle."""
import datetime,hashlib,json,os,signal,subprocess,time,urllib.request
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1');OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
NATIVE=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def age(s):return(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(s.rstrip('Z')+'+00:00')).total_seconds()
def save(n,x):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(x,ensure_ascii=False,indent=2)+'\n');os.replace(t,p)
def rpc(n,d=None,port=8888):
 for a in range(4):
  try:
   q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+n,json.dumps(d or {}).encode(),{'Content-Type':'application/json'})
   with urllib.request.urlopen(q,timeout=15)as f:return json.load(f)
  except Exception:
   if a==3:raise
   time.sleep(.5)
def state(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,Result,ControlGroup,TasksCurrent,NRestarts'],text=True).splitlines()if'='in l)
def memory():return {l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapTotal:','SwapFree:'))}
def process(pid):return{l.split(':')[0]:int(l.split()[1])*1024 for l in(Path('/proc')/str(pid)/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:'))}
def table(name):
 out=[];lower=''
 for _ in range(80):
  v=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':name,'json':True,'limit':5,'time_limit_ms':1000,'lower_bound':lower},8890)
  if not v.get('more'):return out+v['rows']
  nxt=str(v.get('next_key',''))
  if not nxt or nxt==lower:continue
  out+=v['rows'];lower=nxt
 raise TimeoutError(name)
def guard(target=None):
 i=rpc('get_info');f=rpc('get_finalizer_info');rows=table('rotcands')
 assert i['chain_id']==CHAIN and age(i['head_block_time'])<6 and age(i['last_irreversible_block_time'])<25
 assert f['active_finalizer_policy']['threshold']==15 and len(f['active_finalizer_policy']['finalizers'])==21
 assert sorted(x['owner']for x in rows if x['banned'])==BANNED
 recent=[v['description']for v in f['last_tracked_votes']if v['is_vote_strong']and age(v['voted_for_block_timestamp'])<10 and v['description']!=target]
 assert len(recent)>=18,('Need18otherrecentstrongvotes',recent)
 if target:assert next(x for x in rows if x['owner']==target)['zero_windows']==0
 return{'utc':now(),'info':i,'recent_other_strong_votes':recent}

assert not(R/'CANARY_STARTED.json').exists(),'Inspect existing one-shot result before resume'
assert json.loads((R/'CLEANUP_RESULT.json').read_text())['status'].startswith('PASS_')
assert hashlib.sha256(NATIVE.read_bytes()).hexdigest()=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
BANNED=sorted(x['owner']for x in table('rotcands')if x['banned']);assert len(BANNED)==15
eligible={n:x for n,x in nodes.items()if n not in BANNED};assert len(eligible)==21
pre={n:state(x['unit'])for n,x in eligible.items()};assert all(int(s['MainPID'])>0 for s in pre.values())
save('CANARY_STARTED.json',{'utc':now(),'memory':memory(),'protected_before':pre,'guard':guard('berlintucomp'),'profile':{'chain_threads':1,'net_threads':1,'vote_threads_unchanged':True,'parallel_validation_unchanged':True,'MALLOC_ARENA_MAX':2,'MALLOC_TRIM_THRESHOLD_':131072},'source_binary_consensus_unchanged':True})
# appbase's installed SIGHUP handler reloads only logging. No node shutdown.
logchanges=[]
for n,x in eligible.items():
 cfg=Path(x['config_dir'])if'config_dir'in x else OLD/'nodes'/n/'config'
 log=cfg/'logging.json';backup=R/'runtime-backups'/n;backup.mkdir(parents=True,exist_ok=True)
 if log.exists():
  raw=log.read_bytes();(backup/'logging.json').write_bytes(raw);v=json.loads(raw)
 else:
  v={'includes':[],'appenders':[{'name':'stderr','type':'console','args':{'stream':'std_error','level_colors':[]}}],'loggers':[{'name':'default','level':'warn','enabled':True,'additivity':False,'appenders':['stderr']}]}
 for logger in v.get('loggers',[]):
  if logger.get('name')!='deep-mind':logger['level']='warn'
 log.write_text(json.dumps(v,indent=2)+'\n');st=cfg.stat();os.chown(log,st.st_uid,st.st_gid);log.chmod(0o600)
 pid=int(state(x['unit'])['MainPID']);assert pid==int(pre[n]['MainPID']);os.kill(pid,signal.SIGHUP)
 logchanges.append({'name':n,'PID_unchanged':pid,'logging_sha256':hashlib.sha256(log.read_bytes()).hexdigest()})
save('HOT_LOG_RELOAD.json',{'utc':now(),'nodes':logchanges,'all21_PIDs_unchanged':True})
name='berlintucomp';node=eligible[name];root=OLD/'nodes'/name;cfg=root/'config/config.ini';before=cfg.read_bytes()
backup=R/'runtime-backups'/name;(backup/'config.ini').write_bytes(before)
lines=[];changed=[]
for line in before.decode().splitlines():
 if '='in line and line.split('=',1)[0].strip()in['chain-threads','net-threads']:
  k=line.split('=',1)[0].strip();changed.append(k);line=k+' = 1'
 lines.append(line)
assert sorted(changed)==['chain-threads','net-threads']
cfg.write_text('\n'.join(lines)+'\n')
drop=Path('/etc/systemd/system')/(node['unit']+'.d')/'30-local-memory.conf';assert not drop.exists();drop.parent.mkdir(exist_ok=True)
drop.write_text('[Service]\nEnvironment=MALLOC_ARENA_MAX=2\nEnvironment=MALLOC_TRIM_THRESHOLD_=131072\n')
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
g=guard(name);oldpid=int(state(node['unit'])['MainPID']);oldmem=process(oldpid)
save('CANARY_RESTART_STARTED.json',{'utc':now(),'guard':g,'oldPID':oldpid,'oldmemory':oldmem,'unit':node['unit']})
started=time.monotonic();subprocess.run(['systemctl','restart',node['unit']],check=True,capture_output=True,timeout=120)
samples=[];deadline=time.monotonic()+180
while time.monotonic()<deadline:
 try:
  i=rpc('get_info',port=node['rpc_port']);live=rpc('get_info')
  assert i['chain_id']==CHAIN
  if age(i['head_block_time'])<6 and live['head_block_num']-i['head_block_num']<8 and i['last_irreversible_block_num']>=g['info']['last_irreversible_block_num']:
   samples.append({'utc':now(),'info':i})
   if len(samples)>=3:break
 except(OSError,TimeoutError):pass
 time.sleep(2)
assert len(samples)>=3,'Stop rollout; preserve same data/safety and reconcile canary'
s=state(node['unit']);assert int(s['MainPID'])!=oldpid
assert all(state(x['unit'])['MainPID']==pre[n]['MainPID']for n,x in eligible.items()if n!=name)
afterguard=guard(name);result={'status':'PASS_ONE_SAME_DATA_CANARY','utc':now(),'name':name,'unit':node['unit'],'elapsed_seconds':time.monotonic()-started,'oldPID':oldpid,'newPID':int(s['MainPID']),'before_memory':oldmem,'after_memory':process(s['MainPID']),'host_memory':memory(),'state':s,'sync_samples':samples,'after_guard':afterguard,'other20_PIDs_unchanged':True}
save('CANARY_RESULT.json',result);print(json.dumps(result))
