"""Bounded infrastructure monitoring; prioritize current/pending duties without CPU caps or node stops."""
import datetime,fcntl,json,os,subprocess,urllib.request
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1');OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def age(s):return(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(s.rstrip('Z')+'+00:00')).total_seconds()
def rpc(n):
 q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+n,b'{}',{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=5)as f:return json.load(f)
def save(n,d):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n');os.replace(t,p)
def state(u):return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,CPUWeight,IOWeight,NRestarts'],text=True).splitlines()if'='in l)
lock=(R/'priority-watch.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
d={'utc':now(),'actions':[],'alerts':[],'policy':'Only relative CPU/IO weights. No CPU quotas, node restarts/stops, transactions, data deletion or swap allocation.'}
d['memory']={l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapTotal:','SwapFree:'))}
s=os.statvfs('/');d['disk_available_bytes']=s.f_bavail*s.f_frsize
d['pressure']={n:(Path('/proc/pressure')/n).read_text()for n in['cpu','memory','io']}
try:
 i=rpc('get_info');f=rpc('get_finalizer_info');sched=rpc('get_producer_schedule')
 assert i['chain_id']==CHAIN
 d['chain']={k:i[k]for k in['chain_id','head_block_num','head_block_time','last_irreversible_block_num','last_irreversible_block_time']}
 d['head_age_seconds']=age(i['head_block_time']);d['LIB_age_seconds']=age(i['last_irreversible_block_time'])
 d['recent10s_strong_votes']=sum(v['is_vote_strong']and age(v['voted_for_block_timestamp'])<10 for v in f['last_tracked_votes'])
 duties={v['description']for v in f['active_finalizer_policy']['finalizers']}
 if f.get('pending_finalizer_policy'):duties.update(v['description']for v in f['pending_finalizer_policy']['finalizers'])
 for k in['active','pending','proposed']:
  if sched.get(k):duties.update(v['producer_name']for v in sched[k]['producers'])
 can_weight=d['head_age_seconds']<10 and f['active_finalizer_policy']['threshold']>=15 and len(f['active_finalizer_policy']['finalizers'])>=21
 d['priority_duties']=sorted(duties)
 if d['head_age_seconds']>10 or d['LIB_age_seconds']>45:d['alerts'].append('CHAIN_FRESHNESS')
 if d['recent10s_strong_votes']<18:d['alerts'].append('RECENT_STRONG_VOTES_BELOW18')
except Exception as e:
 d['chain_error']=str(e);d['alerts'].append('RPC_ERROR');duties=set();can_weight=False
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
nodes.update({x['name']:x for x in json.loads((R/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
banned=set(json.loads((R/'REPLACEMENT_LIVE_BASELINE.json').read_text())['banned']);nodes={n:x for n,x in nodes.items()if n not in banned}
assert len(nodes)==25
d['BP_processes']={}
for n,x in nodes.items():
 u=x['unit'];st=state(u);pid=st.get('MainPID','0');m={}
 if pid!='0':
  try:m={l.split(':')[0]:int(l.split()[1])*1024 for l in(Path('/proc')/pid/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:'))}
  except OSError:pass
  desired=500 if n in duties or n=='gptgenesisbp' else 250
  # A single staged bootstrap keeps normal weight until its verified sync receipt.
  progressfile=R/'REPLACEMENT_ENROLL_PROGRESS.json'
  if n in {v['name']for v in json.loads((R/'REPLACEMENTS_PREPARED.json').read_text())['nodes']}:
   done={v['name']for v in json.loads(progressfile.read_text()).get('completed',[])} if progressfile.exists()else set()
   if n not in done:desired=500
  if can_weight and(st['CPUWeight']!=str(desired)or st['IOWeight']!=str(desired)):
   subprocess.run(['systemctl','set-property','--runtime',u,'CPUWeight='+str(desired),'IOWeight='+str(desired)],check=True,capture_output=True,timeout=5)
   d['actions'].append({'node':n,'CPUWeight':desired,'IOWeight':desired,'hard_CPU_cap':False})
  if sum(m.values())>1536*1024**2:d['alerts'].append('PROCESS_MEMORY_OVER1536MiB:'+n)
 d['BP_processes'][n]={'state':st,'process_bytes':m}
d['running_BP_count']=sum(v['state']['MainPID']!='0'for v in d['BP_processes'].values())
if d['memory']['MemAvailable']<512*1024**2 or d['memory']['SwapFree']<8*1024**3:d['alerts'].append('MEMORY_HEADROOM')
if d['disk_available_bytes']<20*1024**3:d['alerts'].append('DISK_RESERVE')
if(R/'RECYCLE_BLOCKED.json').exists():d['alerts'].append('MEMORY_RECYCLE_LATCHED')
if(R/'RECYCLE_LATEST.json').exists()and json.loads((R/'RECYCLE_LATEST.json').read_text()).get('status','').startswith('ALERT'):d['alerts'].append('MEMORY_RECYCLE_GUARD_ALERT')
d['status']='ALERT'if d['alerts']else'OK';save('HEALTH_LATEST.json',d)
log=R/'HEALTH_SAMPLES.jsonl'
if log.exists()and log.stat().st_size>8*1024**2:os.replace(log,R/'HEALTH_SAMPLES.previous.jsonl')
with log.open('a')as out:out.write(json.dumps(d)+'\n')
print(json.dumps({'utc':d['utc'],'status':d['status'],'running_BP_count':d['running_BP_count'],'memory':d['memory'],'alerts':d['alerts'],'actions':len(d['actions'])}))
