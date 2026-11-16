"""TEST-only one-at-a-time same-data memory recycling; no chain transactions or dirty-DB repair."""
from pathlib import Path
import datetime,fcntl,hashlib,json,os,subprocess,sys,time,traceback,urllib.request
from stability_policy import select,reconciled,distance
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532';PIN='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397';NATIVE=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def age(t):
 d=datetime.datetime.fromisoformat(t.replace('Z','+00:00'));d=d if d.tzinfo else d.replace(tzinfo=datetime.timezone.utc);return(datetime.datetime.now(datetime.timezone.utc)-d).total_seconds()
def load(n):return json.loads((R/n).read_text())
def save(n,d):
 p=R/n;p.parent.mkdir(mode=0o700,parents=True,exist_ok=True);t=p.with_suffix('.tmp');t.write_text(json.dumps(d,indent=2)+'\n');os.replace(t,p)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def rpc(n,d=None,port=8888):
 assert n.startswith('get_');q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+n,json.dumps(d or{}).encode(),{'Content-Type':'application/json'});return json.load(urllib.request.urlopen(q,timeout=5))
def rows():
 d=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'rotcands','json':True,'limit':1000});assert not d['more'],'Incomplete native ban list';return{v['owner']:v for v in d['rows']}
def state(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState'],text=True).splitlines()if'='in l)
def run(args,timeout=15):return subprocess.run(args,capture_output=True,text=True,check=True,timeout=timeout)
def assert_native_binding(v,pid):
 assert pid!='0';args=[x.decode()for x in(Path('/proc')/pid/'cmdline').read_bytes().split(b'\0')if x]
 assert args[args.index('--config-dir')+1]==v['config_dir']and args[args.index('--data-dir')+1]==v['data_dir'],'Native process binding differs from reviewed node map'
 assert str(NATIVE)in(Path('/proc')/pid/'maps').read_text(),'Unexpected native executable'

def status_context(nodes):
 i=rpc('get_info');assert i['chain_id']==CHAIN;f=rpc('get_finalizer_info');r=rows();assert set(load('GUARD_CONFIG.json')['initial_banned'])<={n for n,x in r.items()if x['banned']},'Ban history changed';schedule=rpc('get_producer_schedule');active=[x['producer_name']for x in schedule['active']['producers']];union=set(active)|{v['description']for v in f['active_finalizer_policy']['finalizers']}
 for k in ['pending','proposed']:
  if schedule.get(k):union|={x['producer_name']for x in schedule[k]['producers']}
 if f.get('pending_finalizer_policy'):union|={v['description']for v in f['pending_finalizer_policy']['finalizers']}
 recent={v['description']for v in f['last_tracked_votes']if v['is_vote_strong']and age(v['voted_for_block_timestamp'])<12};slot=int((datetime.datetime.now(datetime.timezone.utc)-datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)).total_seconds()*2);out={};pids={}
 for n,v in nodes.items():
  assert n in r;pid=state(v['unit'])['MainPID'];pids[n]=pid;amount=0
  if pid!='0':
   try:amount=sum(int(l.split()[1])*1024 for l in(Path('/proc')/pid/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:')))
   except FileNotFoundError:pass
  out[n]={'banned':bool(r[n]['banned']),'strikes':r[n]['zero_windows'],'bytes':amount,'distance':distance(n,active,slot,union),'standby':n not in union}
 mem={l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapFree:'))};last=load('GUARD_LAST.json')if(R/'GUARD_LAST.json').exists()else{};s={'disk_free':os.statvfs('/').f_bavail*os.statvfs('/').f_frsize,'mem_available':mem['MemAvailable'],'head_age':age(i['head_block_time']),'lib_age':age(i['last_irreversible_block_time']),'policy_count':len(f['active_finalizer_policy']['finalizers']),'threshold':f['active_finalizer_policy']['threshold'],'recent':sorted(recent),'all_running':all(pid!='0'for n,pid in pids.items()if not out[n]['banned']),'elapsed':age(last['utc'])if last.get('utc')else 100000,'swap_free':mem['SwapFree'],'nodes':out};return s,i,pids,r,mem

def positive_reconciliation(e,nodes):
 n=e['target'];v=nodes[n];s,i,pids,ro,mem=status_context(nodes);target=rpc('get_info',port=v['rpc_port']);anchor=rpc('get_block',{'block_num_or_id':e['anchor_num']});other={k:p for k,p in pids.items()if k!=n and not ro[k]['banned']};expected={k:p for k,p in e['other_PIDs'].items()if not ro[k]['banned']};proof={'new_pid':pids[n],'old_pid':e['oldPID'],'other_pids_unchanged':other==expected,'anchor_matches':anchor['id']==e['anchor_id'],'target_fresh':target['chain_id']==CHAIN and age(target['head_block_time'])<6 and age(target['last_irreversible_block_time'])<20 and target['last_irreversible_block_num']>=e['anchor_num'],'other_recent':len(set(s['recent'])-{n}),'safety_exists':(Path(v['data_dir'])/'finalizers/safety.dat').is_file(),'config_unchanged':sha(Path(v['config_dir'])/'config.ini')==v['config_sha256'],'banned':bool(ro[n]['banned'])};return reconciled(proof),proof

def perform(name,nodes,ctx):
 s,i,pids,ro,mem=ctx;v=nodes[name];assert_native_binding(v,pids[name]);cfg=Path(v['config_dir'])/'config.ini';safety=Path(v['data_dir'])/'finalizers/safety.dat';assert sha(cfg)==v['config_sha256']and safety.is_file();assert not ro[name]['banned'];assert rpc('get_info',port=v['rpc_port'])['chain_id']==CHAIN
 stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S');event='guard-events/'+stamp+'-'+name;assert not(R/event).exists();watch='pureeos-stability-restore-'+stamp.lower();e={'status':'STARTED','phase':'WATCHDOG_ARMED','utc':now(),'event':event,'target':name,'oldPID':pids[name],'other_PIDs':{n:p for n,p in pids.items()if n!=name and not ro[n]['banned']},'anchor_num':i['last_irreversible_block_num'],'anchor_id':i['last_irreversible_block_id'],'before_bytes':s['nodes'][name]['bytes'],'before_memory':mem,'next_duty_seconds':s['nodes'][name]['distance'],'watchdog':watch};save('GUARD_INFLIGHT.json',e);save(event+'/STARTED.json',e)
 try:
  run(['systemd-run','--unit='+watch,'--on-active=30s','/usr/bin/python3','-B',str(R/'stability_guard.py'),'--restore',name],10);e.update(phase='STOP_REQUESTED',utc=now());save('GUARD_INFLIGHT.json',e)
  run(['systemctl','stop',v['unit']],65);assert state(v['unit'])['MainPID']=='0';raw=safety.read_bytes();assert 0<len(raw)<1024**2;(R/event/'safety.after-clean-stop.dat').write_bytes(raw);e['safety_sha256_before_start']=hashlib.sha256(raw).hexdigest();assert sha(safety)==e['safety_sha256_before_start'];assert not rows()[name]['banned'],'Target became banned; do not restart';e.update(phase='START_REQUESTED',utc=now());save('GUARD_INFLIGHT.json',e);run(['systemctl','start',v['unit']],15)
  end=time.monotonic()+90;good=0;proof={}
  while time.monotonic()<end:
   try:
    okay,proof=positive_reconciliation(e,nodes);good=good+1 if okay else 0
    if good>=3:break
   except Exception:good=0
   time.sleep(2)
  assert good>=3,'Post-start canonical/quorum checks not yet stable; next tick only reconciles this operation'
  run(['systemctl','stop',watch+'.timer']);e.update(status='PASS_ONE_GUARDED_RECYCLE',phase='VERIFIED',utc=now(),proof=proof);save(event+'/RESULT.json',e);save('GUARD_LAST.json',e);save('GUARD_INFLIGHT.json',e);return e
 except BaseException:
  e.update(status='NEEDS_POSITIVE_RECONCILIATION_NO_SECOND_STOP',utc=now(),error=traceback.format_exc());save(event+'/FAILURE.json',e);save('GUARD_INFLIGHT.json',e);return e

def main():
 os.umask(0o077);nodes=load('GUARD_NODE_MAP.json');assert len(nodes)==25;assert load('REPLACEMENT_ENROLL_RESULT.json')['status']=='PASS_6_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS'
 binary_info=NATIVE.stat();stamp=[binary_info.st_dev,binary_info.st_ino,binary_info.st_size,binary_info.st_mtime_ns];pin=load('GUARD_BINARY_VERIFIED.json')if(R/'GUARD_BINARY_VERIFIED.json').exists()else{}
 if pin.get('stat')!=stamp:assert sha(NATIVE)==PIN;save('GUARD_BINARY_VERIFIED.json',{'utc':now(),'sha256':PIN,'stat':stamp})
 if len(sys.argv)==3 and sys.argv[1]=='--restore':
  name=sys.argv[2];assert name in nodes
  if not rows()[name]['banned']:run(['systemctl','start',nodes[name]['unit']],15)
  return
 lock=(R/'native-operation.lock').open('a')
 try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 except BlockingIOError:save('GUARD_LATEST.json',{'status':'SKIP_ANOTHER_NATIVE_OPERATION','utc':now()});return
 e=load('GUARD_INFLIGHT.json')if(R/'GUARD_INFLIGHT.json').exists()else{}
 if e.get('status')in ['STARTED','NEEDS_POSITIVE_RECONCILIATION_NO_SECOND_STOP']:
  good=0;proof={}
  for _ in range(3):
   okay,proof=positive_reconciliation(e,nodes);good=good+1 if okay else 0;time.sleep(2)
  if good==3:
   run(['systemctl','stop',e['watchdog']+'.timer']);e.update(status='PASS_RECONCILED_SAME_OPERATION',phase='VERIFIED',utc=now(),proof=proof);save(e['event']+'/RECONCILED.json',e);save('GUARD_LAST.json',e);save('GUARD_INFLIGHT.json',e)
  save('GUARD_LATEST.json',{'status':e['status'],'utc':now(),'target':e['target'],'proof':proof,'scope':'No second restart while an operation remains unverified'});return
 from retire_banned_runtime import retire_confirmed
 retired=retire_confirmed(sys.modules[__name__],nodes)
 if retired:save('GUARD_LATEST.json',{'status':'RETIRED_NEW_PERMANENT_BANS','utc':now(),'retired':retired});return
 ctx=status_context(nodes);s,i,pids,ro,mem=ctx;name,reason=select(s);d={'status':reason,'utc':now(),'memory':mem,'head':i['head_block_num'],'LIB':i['last_irreversible_block_num'],'fresh_strong_votes':len(s['recent']),'target':name,'over768MiB':{n:v['bytes']for n,v in s['nodes'].items()if v['bytes']>768*1024**2},'policy':{'limit_MiB':768,'emergency_MiB':512,'cooldown_seconds':180,'one_native_operation':True,'historical_strikes_preserved':True}}
 if name and '--check-only'not in sys.argv:
  # Re-read the complete qualification before arming a stop.
  again=status_context(nodes);target,_=select(again[0])
  if target==name:d.update(operation=perform(name,nodes,again));d['status']=d['operation']['status']
  else:d['status']='SKIP_CONTEXT_CHANGED_BEFORE_STOP'
 elif name:d['status']='CHECK_ONLY_WOULD_RECYCLE_ONE'
 save('GUARD_LATEST.json',d);print(json.dumps(d),flush=True)
if __name__=='__main__':
 try:main()
 except Exception:save('GUARD_LATEST.json',{'status':'ALERT_READ_OR_PREFLIGHT_RETRY_NO_NEW_STOP','utc':now(),'error':traceback.format_exc()});raise
