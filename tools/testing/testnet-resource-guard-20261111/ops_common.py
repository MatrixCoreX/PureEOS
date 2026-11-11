"""Scoped helpers for this Nov11 operation; importing performs no service or financial actions."""
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

