"""Cost-bounded TEST-only preventive recycle; never changes chain state, keys or safety records."""
import datetime,fcntl,hashlib,json,os,subprocess,sys,time,urllib.request
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1');OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
NATIVE=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
LIMIT=1280*1024**2;COOLDOWN=600;CHECK_ONLY='--check-only'in sys.argv;os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def age(s):
 stamp=datetime.datetime.fromisoformat(s.replace('Z','+00:00'))
 if stamp.tzinfo is None:stamp=stamp.replace(tzinfo=datetime.timezone.utc)
 return(datetime.datetime.now(datetime.timezone.utc)-stamp).total_seconds()
def save(n,d):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n');os.replace(t,p)
def rpc(n,d=None,port=8888):
 assert n.startswith('get_'),'No transaction or producer-mutation RPC allowed'
 q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+n,json.dumps(d or {}).encode(),{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=8)as f:return json.load(f)
def state(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState'],text=True).splitlines()if'='in l)
def table(n):
 out=[];lower=''
 for _ in range(60):
  v=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':n,'json':True,'limit':5,'time_limit_ms':1000,'lower_bound':lower},8890)
  if not v.get('more'):return out+v['rows']
  nxt=str(v.get('next_key',''))
  if not nxt or nxt==lower:continue
  out+=v['rows'];lower=nxt
 raise RuntimeError('Incomplete native table')
def finish(d):
 d['utc']=now();save('RECYCLE_CHECK_ONLY.json'if CHECK_ONLY else'RECYCLE_LATEST.json',d);print(json.dumps(d));sys.exit(0)
lock=(R/'operational-restart.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
event=None
d={'utc':now(),'check_only':CHECK_ONLY,'threshold_bytes':LIMIT,'cooldown_seconds':COOLDOWN,'source_binary_chain_rules_unchanged':True}
try:
 if(R/'RECYCLE_BLOCKED.json').exists():finish({**d,'status':'ALERT_LATCHED_REQUIRES_MANUAL_RECONCILIATION'})
 assert json.loads((R/'REPLACEMENT_ENROLL_RESULT.json').read_text())['status']=='PASS_4_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS'
 if(R/'RECYCLE_LAST_ATTEMPT.json').exists():
  previous=json.loads((R/'RECYCLE_LAST_ATTEMPT.json').read_text())
  if previous['status']=='ATTEMPT_STARTED':
   blocked={**d,'status':'ALERT_UNRECONCILED_PREVIOUS_ATTEMPT'};save('RECYCLE_BLOCKED.json',blocked);finish(blocked)
  if age(previous['utc'])<COOLDOWN:finish({**d,'status':'NO_ACTION_COOLDOWN'})
 baseline=json.loads((R/'REPLACEMENT_LIVE_BASELINE.json').read_text());banned=set(baseline['banned'])
 nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']});nodes={n:x for n,x in nodes.items()if n not in banned};nodes.update({x['name']:x for x in json.loads((R/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
 assert len(nodes)==25
 pids={n:state(x['unit'])['MainPID']for n,x in nodes.items()};assert all(p!='0'for p in pids.values()),'Missing BP; no automated second outage'
 amounts={}
 for n,p in pids.items():
  vals={l.split(':')[0]:int(l.split()[1])*1024 for l in(Path('/proc')/p/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:'))};amounts[n]=sum(vals.values())
 d['largest_process_bytes']=max(amounts.values());d['over_threshold']={n:v for n,v in amounts.items()if v>LIMIT}
 # Even below the memory limit, validate the real native safety decision path.
 rows=table('rotcands');assert len(rows)==41 and{x['owner']for x in rows if x['banned']}==banned,'New native ban: manual audit required'
 f=rpc('get_finalizer_info');schedule=rpc('get_producer_schedule');i=rpc('get_info')
 assert i['chain_id']==CHAIN and age(i['head_block_time'])<5 and age(i['last_irreversible_block_time'])<15,'Stale chain: no recycling'
 assert f['active_finalizer_policy']['threshold']==15 and len(f['active_finalizer_policy']['finalizers'])==21
 active=[v['producer_name']for v in schedule['active']['producers']]
 union=set(active)|{v['description']for v in f['active_finalizer_policy']['finalizers']}
 for k in['pending','proposed']:
  if schedule.get(k):union.update(v['producer_name']for v in schedule[k]['producers'])
 if f.get('pending_finalizer_policy'):union.update(v['description']for v in f['pending_finalizer_policy']['finalizers'])
 recent={v['description']for v in f['last_tracked_votes']if v['is_vote_strong']and age(v['voted_for_block_timestamp'])<10};assert len(recent)>=19
 epoch=datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc);slot=int((datetime.datetime.now(datetime.timezone.utc)-epoch).total_seconds()*2);turn=(slot//12)%len(active)
 def distance(n):return 1000 if n not in union else (((active.index(n)-turn)%len(active))*12-slot%12)/2 if n in active else 0
 candidates=[n for n in amounts if amounts[n]>LIMIT and next(r for r in rows if r['owner']==n)['zero_windows']==0 and distance(n)>=90 and len(recent-{n})>=18]
 d.update(head=i['head_block_num'],LIB=i['last_irreversible_block_num'],recent_strong_votes=len(recent),eligible_candidates=candidates)
 if not d['over_threshold']:finish({**d,'status':'NO_ACTION_ALL_BELOW1280MiB'})
 if not candidates:finish({**d,'status':'NO_ACTION_WAIT_SAFE_DUTY_WINDOW'})
 name=max(candidates,key=lambda n:(n not in union,amounts[n]));node=nodes[name];d.update(target=name,next_duty_seconds=distance(name),other_strong_votes=len(recent-{name}))
 if CHECK_ONLY:finish({**d,'status':'CHECK_ONLY_WOULD_RECYCLE_ONE'})
 assert hashlib.sha256(NATIVE.read_bytes()).hexdigest()=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
 current=rpc('get_info',port=node['rpc_port']);assert current['chain_id']==CHAIN and age(current['head_block_time'])<5
 stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S');event=R/'recycle-events'/stamp;event.mkdir(parents=True,mode=0o700)
 d.update(status='ATTEMPT_STARTED',oldPID=pids[name],before_process_bytes=amounts[name]);save('RECYCLE_LAST_ATTEMPT.json',d);(event/'STARTED.json').write_text(json.dumps(d,indent=2)+'\n')
 watchdog='pureeos-recycle-start-safeguard-'+stamp.lower()
 subprocess.run(['systemd-run','--unit='+watchdog,'--on-active=60s','/usr/bin/systemctl','start',node['unit']],check=True,capture_output=True,timeout=10)
 subprocess.run(['systemctl','stop',node['unit']],check=True,capture_output=True,timeout=45)
 data=Path(node['data_dir'])if'data_dir'in node else OLD/'nodes'/name/'data'if name in{'berlintucomp','bostonmiteng','nvidiaailabs','tencentailab','zurichethnet'}else R/'nodes'/name/'data'
 safety=data/'finalizers/safety.dat';raw=safety.read_bytes();assert 0<len(raw)<1024**2;(event/'safety.after-clean-stop.dat').write_bytes(raw);d['safety_before_start_sha256']=hashlib.sha256(raw).hexdigest()
 assert hashlib.sha256(safety.read_bytes()).hexdigest()==d['safety_before_start_sha256']
 subprocess.run(['systemctl','start',node['unit']],check=True,capture_output=True,timeout=15)
 deadline=time.monotonic()+60;good=0
 while time.monotonic()<deadline:
  try:
   current=rpc('get_info',port=node['rpc_port'])
   if current['chain_id']==CHAIN and age(current['head_block_time'])<5 and current['last_irreversible_block_num']>=i['last_irreversible_block_num']:
    good+=1
    if good>=3:break
  except Exception:pass
  time.sleep(2)
 assert good>=3,'Leave safety/data intact; independent start safeguard remains armed'
 assert all(state(nodes[n]['unit'])['MainPID']==p for n,p in pids.items()if n!=name)
 assert state(node['unit'])['MainPID']not in['0',pids[name]]
 f=rpc('get_finalizer_info');assert sum(v['is_vote_strong']and age(v['voted_for_block_timestamp'])<10 and v['description']!=name for v in f['last_tracked_votes'])>=18
 subprocess.run(['systemctl','stop',watchdog+'.timer'],check=True,capture_output=True)
 d.update(status='PASS_ONE_GUARDED_RECYCLE',newPID=state(node['unit'])['MainPID'],other24_PIDs_unchanged=True,native_safety_forward_updates_allowed=True);(event/'RESULT.json').write_text(json.dumps(d,indent=2)+'\n');save('RECYCLE_LAST_ATTEMPT.json',d);finish(d)
except SystemExit:raise
except Exception as e:
 failure={**d,'status':'ALERT_NO_FURTHER_AUTOMATIC_ACTION','error':str(e)}
 if event is not None:
  save('RECYCLE_BLOCKED.json',failure);(event/'FAILURE.json').write_text(json.dumps(failure,indent=2)+'\n')
 finish(failure)
