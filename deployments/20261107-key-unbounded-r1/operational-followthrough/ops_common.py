"""Helpers for this new upgrade only. Never replays historical controllers."""
from pathlib import Path
import datetime,hashlib,json,os,subprocess,time,urllib.request
R=Path('/opt/pureeos-key-unbounded-20261107-r1')
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
OLD='30f6e44225419c30c180a89195c8ace90f4b24a193d868abc447cbb348ad09ae'
NEW='7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416'
SIGNUP='3c03d8671479cb204e5c4ae31ae0563883882417cf88f8c3c477bd7e1e1a5791'
SYSTEM='b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
TOKEN='f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
ISSUER='ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb'
sha=lambda b:hashlib.sha256(b).hexdigest()
def save(path,value):
 path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
 temporary=path.with_name(path.name+'.tmp');raw=(json.dumps(value,ensure_ascii=False,indent=2)+'\n').encode()
 with temporary.open('wb')as stream:stream.write(raw);stream.flush();os.fsync(stream.fileno())
 os.replace(temporary,path)
 fd=os.open(path.parent,os.O_RDONLY)
 try:os.fsync(fd)
 finally:os.close(fd)
def rpc(method,data=None,port=8888):
 request=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/{method}',data=json.dumps(data).encode()if data else b'',headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(request,timeout=30)as f:return json.load(f)
def stamp(value):
 result=datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
 return result.replace(tzinfo=datetime.timezone.utc)if result.tzinfo is None else result

def native_unit(unit):
 return unit.startswith(('pureeos-devnet-bp-','pureeos-bp-demo-','pureeos-bp-extra-'))or unit in ['pureeos-devnet-founder-20261105.service','pureeos-devnet-history-20261105.service']

def process_guard():
 baseline=json.loads((R/'LIVE_BASELINE.json').read_text());current={}
 for unit,b in baseline['services'].items():
  pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True))
  if not pid and unit=='pureeos-devnet-ordinary-activity-20261105.service':
   deadline=time.monotonic()+30
   while not pid and time.monotonic()<deadline:
    time.sleep(.5);pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True))
  assert pid>0,unit
  p=Path('/proc')/str(pid);row={'PID':pid,'start_ticks':(p/'stat').read_text().split()[21],'cmdline_sha256':sha((p/'cmdline').read_bytes())}
  if native_unit(unit):assert row['PID']==b['PID']and row['start_ticks']==b['start_ticks']and row['cmdline_sha256']==b['cmdline_sha256'],('Native process changed',unit)
  elif unit=='pureeos-devnet-ordinary-activity-20261105.service':assert row['cmdline_sha256']==b['cmdline_sha256'],('Ordinary command identity changed',unit)
  current[unit]=row
 return current

def guard():
 info=rpc('get_info');assert info['chain_id']==CHAIN
 assert(datetime.datetime.now(datetime.timezone.utc)-stamp(info['head_block_time'])).total_seconds()<60
 assert info['head_block_num']-info['last_irreversible_block_num']<128
 process_guard()
 return info

def code_snapshots():
 return {n:rpc('get_raw_abi',{'account_name':n})for n in ['eosio.key','eosio.signup','eosio','eosio.token','usd','cny']}

def require_codes(key_code):
 expected={'eosio.key':key_code,'eosio.signup':SIGNUP,'eosio':SYSTEM,'eosio.token':TOKEN,'usd':ISSUER,'cny':ISSUER}
 for account,code in expected.items():assert rpc('get_code_hash',{'account_name':account})['code_hash']==code,(account,'Unexpected code')

def rows(code,scope,table,extra=None):
 result=[];lower=str((extra or{}).get('lower_bound',''));seen=set();partial=0
 for _ in range(1000):
  q={'json':True,'code':code,'scope':str(scope),'table':table,'limit':1000,'lower_bound':lower,'time_limit_ms':500}
  q.update(extra or{});q['lower_bound']=lower;page=rpc('get_table_rows',q);next_key=str(page.get('next_key',''))
  if page.get('more')and(not next_key or next_key==lower):
   partial+=1;assert partial<=5,('Repeated nonadvancing page',table);time.sleep(.1);continue
  result.extend(page['rows']);partial=0
  if not page.get('more'):return result
  assert next_key not in seen,('Repeated table cursor',table);seen.add(next_key);lower=next_key
 raise RuntimeError('Bounded read exceeded; no partial audit result')

def authority_guard():
 baseline=json.loads((R/'PRE_UPGRADE_AUTHORITIES.json').read_text())
 for name,permissions in baseline.items():
  current=rpc('get_account',{'account_name':name});assert not current['privileged']and current['permissions']==permissions,('Authority changed',name)

def receipt(txid,start,timeout=600):
 deadline=time.monotonic()+timeout;number=start+1;previous=rpc('get_block',{'block_num_or_id':start})['id']
 while time.monotonic()<deadline:
  info=rpc('get_info')
  while number<=info['last_irreversible_block_num']:
   block=rpc('get_block',{'block_num_or_id':number});assert block['previous']==previous,'Canonical link mismatch';previous=block['id']
   matches=[t for t in block['transactions']if(t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==txid]
   if matches:
    assert len(matches)==1 and matches[0]['status']=='executed'
    return {'transaction_id':txid,'block_num':number,'block_id':block['id'],'timestamp':block['timestamp'],'irreversible_at_observed_LIB':info['last_irreversible_block_num']}
   number+=1
  time.sleep(2)
 raise RuntimeError('Canonical receipt still unknown: preserve original packet, no new signature')
