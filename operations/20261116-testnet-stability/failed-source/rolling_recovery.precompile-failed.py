from pathlib import Path
import json,subprocess,urllib.request,datetime,time,hashlib,os,traceback,fcntl
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def age(t):return(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(t.rstrip('Z')+'+00:00')).total_seconds()
def save(n,d):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(d,indent=2)+'\n');os.replace(t,p)
def rpc(n,d=None,port=8888):
 q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+n,json.dumps(d or{}).encode(),{'Content-Type':'application/json'});return json.load(urllib.request.urlopen(q,timeout=8))
def st(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,NRestarts'],text=True).splitlines()if'='in l)
def mem(pid):return sum(int(l.split()[1])*1024 for l in(Path('/proc')/pid/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:')))
def memory():return {l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapFree:'))}
def table():
 v=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'rotcands','json':True,'limit':1000});assert not v['more'];return v['rows']
def guard():
 i=rpc('get_info');f=rpc('get_finalizer_info');assert i['chain_id']==CHAIN and age(i['head_block_time'])<6 and age(i['last_irreversible_block_time'])<20
 rows=table();assert{x['owner']for x in rows if x['banned']}==bans
 recent={v['description']for v in f['last_tracked_votes']if v['is_vote_strong']and age(v['voted_for_block_timestamp'])<12};assert len(recent)>=f['active_finalizer_policy']['threshold']+3
 return i,f,rows,recent
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532';binary=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos');assert hashlib.sha256(binary.read_bytes()).hexdigest()=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
assert not(R/'ROLL_STARTED.json').exists();lock=(R/'native-operation.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);plan=json.loads((R/'BANNED_STOP_PLAN.json').read_text());bans=set(plan['banned']);nodes={n:dict(v)for n,v in plan['all_known_nodes'].items()if n not in bans};assert len(nodes)==19
for n,v in nodes.items():
 if 'data_dir'not in v:
  parent=Path('/opt/pureeos-testnet-resilience-20261110-r1/nodes')if n in {'berlintucomp','bostonmiteng','nvidiaailabs','tencentailab','zurichethnet'}else Path('/opt/pureeos-testnet-new-ban-20261111-r1/nodes');v['data_dir']=str(parent/n/'data');v['config_dir']=str(parent/n/'config')
 v['PID']=st(v['unit'])['MainPID'];assert v['PID']!='0'
# Normal graceful stops must never degrade into automatic SIGKILL.
for v in nodes.values():
 d=Path('/etc/systemd/system')/(v['unit']+'.d');d.mkdir(exist_ok=True);p=d/'96-stability-graceful-20261116.conf';assert not p.exists();p.write_text('[Service]\nTimeoutStopSec=infinity\n')
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
i,f,_,_=guard();progress={'status':'ROLLING','utc':now(),'initial_memory':memory(),'initial_info':i,'nodes':nodes,'completed':[]};save('ROLL_STARTED.json',progress);save('LIVE_NODES.json',nodes)
try:
 deadline=time.monotonic()+1400
 while len(progress['completed'])<len(nodes):
  assert time.monotonic()<deadline,'Bounded rolling deadline; leave all remaining services running'
  done={x['name']for x in progress['completed']};pending=[n for n in nodes if n not in done]
  try:i,f,rows,recent=guard()
  except Exception:time.sleep(3);continue
  sched=rpc('get_producer_schedule')['active']['producers'];active=[v['producer_name']for v in sched];slot=int((datetime.datetime.now(datetime.timezone.utc)-datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)).total_seconds()*2);turn=(slot//12)%len(active)
  def distance(n):return 999 if n not in active else (((active.index(n)-turn)%len(active))*12-slot%12)/2
  available=[n for n in pending if next(r for r in rows if r['owner']==n)['zero_windows']==0 and distance(n)>=65 and len(recent-{n})>=f['active_finalizer_policy']['threshold']+2]
  # Keep the writer until all other native processes have been qualified.
  if len(pending)>1:available=[n for n in available if n!='gptgenesisbp']
  if not available:time.sleep(2);continue
  name=max(available,key=lambda n:mem(st(nodes[n]['unit'])['MainPID']));v=nodes[name];old=st(v['unit'])['MainPID'];others={n:st(x['unit'])['MainPID']for n,x in nodes.items()if n!=name};event=R/'roll-events'/name;event.mkdir(parents=True,mode=0o700);record={'name':name,'utc':now(),'oldPID':old,'before_bytes':mem(old),'before_info':i,'next_duty_seconds':distance(name),'recent_other_strong':len(recent-{name})};(event/'STARTED.json').write_text(json.dumps(record,indent=2));progress['current']=record;save('ROLL_PROGRESS.json',progress)
  safety=Path(v['data_dir'])/'finalizers/safety.dat';assert safety.is_file()
  watch='pureeos-stability-start-'+name+'-20261116';subprocess.run(['systemd-run','--unit='+watch,'--on-active=60s','/usr/bin/systemctl','start',v['unit']],check=True,capture_output=True,timeout=10)
  subprocess.run(['systemctl','stop',v['unit']],check=True,capture_output=True,timeout=65);assert st(v['unit'])['MainPID']=='0';raw=safety.read_bytes();(event/'safety.after-clean-stop.dat').write_bytes(raw);digest=hashlib.sha256(raw).hexdigest();assert hashlib.sha256(safety.read_bytes()).hexdigest()==digest
  subprocess.run(['systemctl','start',v['unit']],check=True,capture_output=True,timeout=20)
  until=time.monotonic()+90;good=0
  while time.monotonic()<until:
   try:
    target=rpc('get_info',port=v['rpc_port']);fin=rpc('get_finalizer_info',port=8890 if name=='gptgenesisbp'else8888);rec={x['description']for x in fin['last_tracked_votes']if x['is_vote_strong']and age(x['voted_for_block_timestamp'])<15}
    if target['chain_id']==CHAIN and age(target['head_block_time'])<8 and target['last_irreversible_block_num']>=i['last_irreversible_block_num']and len(rec-{name})>=fin['active_finalizer_policy']['threshold']+2:good+=1
    else:good=0
    if good>=3:break
   except Exception:good=0
   time.sleep(2)
  assert good>=3,'Target not fresh/confirmed; preserve data+safety and independent start watchdog'
  assert all(st(nodes[n]['unit'])['MainPID']==pid for n,pid in others.items());new=st(v['unit'])['MainPID'];assert new not in ['0',old];subprocess.run(['systemctl','stop',watch+'.timer'],capture_output=True,check=True)
  record.update(status='PASS_ONE_CLEAN_SAME_DATA_RESTART',newPID=new,after_bytes=mem(new),safety_before_start_sha256=digest,final_info=target,other_PIDs_unchanged=True);(event/'RESULT.json').write_text(json.dumps(record,indent=2));progress['completed'].append(record);progress['utc']=now();save('ROLL_PROGRESS.json',progress)
 progress.update(status='PASS_ALL19_SAME_DATA_RESTARTED',utc=now(),final_memory=memory(),final_info=rpc('get_info'));save('ROLL_RESULT.json',progress);print(json.dumps({'status':progress['status'],'memory':progress['final_memory']}),flush=True)
except BaseException:
 progress.update(status='HALTED_RECONCILE_CURRENT_NODE_BEFORE_CONTINUING',utc=now(),error=traceback.format_exc());save('ROLL_FAILURE.json',progress);raise
