"""Helpers for this authorized current-chain demonstration only."""
from pathlib import Path
import datetime,json,os,subprocess,time,hashlib,urllib.request,urllib.error,socket
os.umask(0o077)
R=Path('/opt/pureeos-bp-add-two-stop-two-20261106-r1');C=Path('/opt/pureeos-devnet-20261105-r1')
V=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5')
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
BINARY='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
CLEOS=str(C/'tools-runtime/programs/cleos/cleos');UTIL=str(C/'tools-runtime/programs/helios-util/helios-util')
UNIT='pureeos-devnet-wallet-20261105.service';BANK='civicbankers'
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(name,value):
 p=R/name;p.parent.mkdir(parents=True,exist_ok=True);temp=p.with_name(p.name+'.tmp');temp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n');os.replace(temp,p)
def read(name):return json.loads((R/name).read_text())
def rpc(name,data={},port=8888):
 assert name.startswith('get_'),'Read helper cannot submit transactions'
 for attempt in range(4):
  try:
   q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+name,json.dumps(data).encode(),{'Content-Type':'application/json'})
   with urllib.request.urlopen(q,timeout=20)as f:return json.load(f)
  except urllib.error.HTTPError as e:
   body=e.read().decode(errors='replace')
   if e.code not in[500,502,503,504]or attempt==3:raise RuntimeError(name+' HTTP'+str(e.code)+' '+body[:1500])from e
   time.sleep(.2*(attempt+1))
def table(name,code='eosio',scope='eosio'):
 rows=[];lower='';deadline=time.monotonic()+90
 while time.monotonic()<deadline:
  v=rpc('get_table_rows',{'code':code,'scope':scope,'table':name,'json':True,'limit':1000,'lower_bound':lower})
  if not v.get('more'):
   rows+=v['rows'];return rows
  following=str(v.get('next_key',''))
  if not following or following==lower:
   # A time-limited response can contain rows without a usable continuation.
   # Discard this incomplete page before retrying its original lower bound.
   # Appending it first would duplicate rows when the retry succeeds.
   with(R/'TABLE_READ_RETRIES.jsonl').open('a')as out:out.write(json.dumps({'UTC':now(),'table':name,'code':code,'scope':scope,'lower_bound':lower,'returned_rows':len(v['rows']),'next_key':following,'reason':'Non-advancing incomplete page discarded; retry same lower bound'})+'\n')
   time.sleep(.2);continue
  rows+=v['rows'];lower=following
 raise RuntimeError('Table pagination deadline '+name)
def state(unit):
 return dict(x.split('=',1)for x in subprocess.check_output(['systemctl','show',unit,'-p','ActiveState,SubState,MainPID,NRestarts,UnitFileState'],text=True).splitlines()if'='in x)
def balance(account,code='eosio.token',symbol='AIC'):
 values=rpc('get_currency_balance',{'code':code,'account':account,'symbol':symbol});return int(values[0].split()[0].replace('.',''))if values else 0
def action(account,name,actor,data):return {'account':account,'name':name,'authorization':[{'actor':actor,'permission':'active'}],'data':data}
def sysact(name,actor,data):return action('eosio',name,actor,data)
def transfer(sender,to,quantity,memo):return action('eosio.token','transfer',sender,{'from':sender,'to':to,'quantity':quantity,'memo':memo})
def permission(key):return {'threshold':1,'keys':[{'key':key,'weight':1}],'accounts':[],'waits':[]}
def canonical(tid,start,timeout=600):
 deadline=time.monotonic()+timeout;cursor=start+1
 while time.monotonic()<deadline:
  info=rpc('get_info');assert info['chain_id']==CHAIN
  while cursor<=info['last_irreversible_block_num']:
   b=rpc('get_block',{'block_num_or_id':cursor})
   for t in b['transactions']:
    if(t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==tid:
     assert t['status']=='executed';return {'transaction_id':tid,'block_num':cursor,'block_id':b['id'],'observed_LIB':info['last_irreversible_block_num'],'CPU_us':t['cpu_usage_us'],'NET_words':t['net_usage_words']}
   cursor+=1
  time.sleep(1)
 raise RuntimeError('Reconcile original immutable ID '+tid+'; never automatically re-sign or repeat')
def original_pids():
 p=read('BASELINE.json')['node_pids'];current={u:int(state(u)['MainPID'])for u in p};assert current==p,'Original node PID changed';return current
def new_key(bls=False):
 command=[UTIL,'bls','create','key','--to-console']if bls else[CLEOS,'create','key','--to-console']
 q=subprocess.run(command,capture_output=True,text=True,check=True);v={}
 for line in q.stdout.splitlines():
  if ': 'in line:
   k,value=line.split(': ',1)
   if k in ['Private key','Public key','Proof of Possession']:v[k]=value.strip()
 assert set(v)==({'Private key','Public key','Proof of Possession'}if bls else{'Private key','Public key'})
 return v
class Wallet:
 def __init__(self):self.owned=False;self.secrets=[]
 def __enter__(self):
  assert state(UNIT)['ActiveState']=='inactive','Preserve an unrelated existing signer session'
  subprocess.run(['systemctl','start',UNIT],check=True);self.owned=True;deadline=time.monotonic()+60
  while True:
   s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
   try:s.settimeout(1);s.connect(str(C/'founder/wallet/keosd.sock'));break
   except OSError:assert time.monotonic()<deadline;time.sleep(.2)
   finally:s.close()
  password=(C/'founder/wallet/password.txt').read_text().strip();self.secrets.append(password)
  q=self.command(['wallet','unlock','-n','localtest'],input_text=password+'\n',allow_failure=True)
  assert q.returncode==0 or 'already unlocked'in(q.stdout+q.stderr).lower(),'Existing wallet unlock failed'
  return self
 def __exit__(self,*args):
  if self.owned:subprocess.run(['systemctl','stop',UNIT],check=True)
  save('LAST_SIGNER_CLOSED.json',{'status':'PASS','UTC':now(),'unit':UNIT,'inactive':state(UNIT)['ActiveState']=='inactive'})
 def redact(self,value):
  for secret in self.secrets:value=value.replace(secret,'[REDACTED]')
  return value
 def command(self,args,input_text=None,allow_failure=False):
  q=subprocess.run([CLEOS,'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(C/'founder/wallet/keosd.sock'),*args],input=input_text,capture_output=True,text=True,timeout=90)
  if q.returncode and not allow_failure:raise RuntimeError(self.redact((q.stdout+q.stderr)[:5000]))
  return q
 def import_key(self,key):
  self.secrets.append(key['Private key']);q=self.command(['wallet','import','-n','localtest'],input_text=key['Private key']+'\n',allow_failure=True)
  assert q.returncode==0 or 'already in wallet'in(q.stdout+q.stderr).lower(),'Owned new key import failed'
 def send(self,label,actions):
  assert all(c.isalnum()or c in '-_'for c in label)
  plan=read('PLAN.json');owned={x['name']for x in plan['nodes']}|{x['voter']for x in plan['nodes']}
  assert all(a['account']in['eosio','eosio.token']and all(p['actor']in owned|{BANK}and p['permission']=='active'for p in a['authorization'])for a in actions)
  for a in actions:
   d=a['data']
   if a['account']=='eosio.token':assert a['name']=='transfer'and d['to']in owned and d['from']==BANK
   else:
    assert a['name']in['newaccount','buyram','delegatebw','powerup','bondbp','regproducer','regfinkey','regpeerkey','voteproducer']
    if a['name']=='delegatebw':assert d['from']==d['receiver']and d['from']in owned and not d['transfer']
    if a['name']=='powerup':assert d['payer']==d['receiver']and d['payer']in owned
    if a['name']=='newaccount':assert d['creator']==BANK and d['name']in owned
    if a['name']=='buyram':assert d['payer']==BANK and d['receiver']in owned
    if a['name']in['bondbp','regproducer','settlebond']:assert d['producer']in owned
    if a['name']=='voteproducer':assert d['voter']in owned and not d['proxy']and len(d['producers'])==1 and d['producers'][0]in owned
    if a['name']=='setmaint':assert d['producer']==plan['maintenance_target']and d['maintenance']is True
  packet=R/'private/packets'/label;packet.mkdir(parents=True,exist_ok=True);progress=R/'transactions'/(label+'.json')
  intent={'actions':actions}
  if progress.exists():
   old=json.loads(progress.read_text());assert json.loads((packet/'intent.json').read_text())==intent
   if old['status']=='IRREVERSIBLE':return old['canonical_receipt']
   raise RuntimeError('Existing transaction '+label+' requires canonical reconciliation; no blind replay')
  (packet/'intent.json').write_text(json.dumps(intent))
  q=self.command(['push','transaction',str(packet/'intent.json'),'-j','-x','1800','--dont-broadcast','--compression','zlib'])
  signed=json.loads(q.stdout);assert signed['signatures']and len(signed['actions'])==len(actions)
  path=packet/'signed.json';path.write_text(json.dumps(signed,indent=2)+'\n')
  packed=json.loads(self.command(['convert','pack_transaction',str(path)]).stdout);assert packed['compression']in[0,'none']
  tid=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest();start=rpc('get_info')['last_irreversible_block_num']
  p={'status':'SIGNED_IMMUTABLE','UTC':now(),'transaction_id':tid,'start_LIB':start,'packet_SHA256':hashlib.sha256(path.read_bytes()).hexdigest(),'attempts':[]};save('transactions/'+label+'.json',p)
  for attempt in range(1,4):
   p['status']='BROADCASTING';save('transactions/'+label+'.json',p)
   q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/push_transaction',json.dumps(packed).encode(),{'Content-Type':'application/json'})
   try:
    with urllib.request.urlopen(q,timeout=90)as f:response=json.load(f)
    (packet/('response-'+str(attempt)+'.json')).write_text(json.dumps(response))
    assert response['transaction_id']==tid and response['processed']['receipt']['status']=='executed'
    p.update(status='ACCEPTED_AWAITING_IRREVERSIBILITY',reported_block=response['processed']['block_num']);save('transactions/'+label+'.json',p);break
   except urllib.error.HTTPError as e:
    error=json.loads(e.read());save('transactions/'+label+'-rejection-'+str(attempt)+'.json',{'transaction_id':tid,'error':error,'UTC':now()})
    assert error.get('error',{}).get('name')=='tx_cpu_usage_exceeded','Native refusal; preserve exact packet'
    target=rpc('get_info')['head_block_num']+2;deadline=time.monotonic()+180
    while rpc('get_info')['last_irreversible_block_num']<target:assert time.monotonic()<deadline;time.sleep(1)
    end=rpc('get_info')['last_irreversible_block_num'];absent=[]
    for n in range(start+1,end+1):
     b=rpc('get_block',{'block_num_or_id':n});assert all((t['trx']if isinstance(t['trx'],str)else t['trx']['id'])!=tid for t in b['transactions']);absent.append({'block_num':n,'block_id':b['id']})
    p['attempts'].append({'attempt':attempt,'CPU_rejected_canonical_absent':absent,'same_packet':True});p['status']='CPU_REJECTED_CANONICAL_ABSENT';save('transactions/'+label+'.json',p);assert attempt<3
   except (TimeoutError,OSError,urllib.error.URLError):
    p['status']='AMBIGUOUS_RECONCILING_ORIGINAL_ID';save('transactions/'+label+'.json',p);break
  proof=canonical(tid,start);p.update(status='IRREVERSIBLE',canonical_receipt=proof);save('transactions/'+label+'.json',p);return proof
