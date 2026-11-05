"""New bridge upgrade only: exact published review, developer-only signing, immutable packets.
Run REMOTELY. Never controls node/BP/history/business units or a founder wallet.
"""
from pathlib import Path
import base64,datetime,hashlib,http.client,json,os,socket,subprocess,sys,time,urllib.request
os.umask(0o077)
R=Path('/opt/pureeos-wallet-signup-bridge-20261105-r1');C=Path('/opt/pureeos-android-wallet-20261104-r1/newchain-20261105-r1');CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
kind,operation=sys.argv[1:];assert kind in ['signup','key']and operation in ['propose','execute'];G=R/'governance'/kind;G.mkdir(mode=0o700,parents=True,exist_ok=True)
sha=lambda b:hashlib.sha256(b).hexdigest()
def save(p,v):p.write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n')
def rpc(method,data={}):
 req=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=45)as f:return json.load(f)
def guard():
 info=rpc('get_info');assert info['chain_id']==CHAIN
 age=(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(info['head_block_time']+'+00:00')).total_seconds();assert age<60 and info['head_block_num']-info['last_irreversible_block_num']<128
 for unit,b in json.loads((C/'public/BASELINE.json').read_text())['current_nodes_by_unit'].items():
  pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True));assert pid==b['PID'],unit
  p=Path('/proc')/str(pid);assert(p/'stat').read_text().split()[21]==b['start_ticks']and sha((p/'cmdline').read_bytes())==b['cmdline_sha256'],unit
 return info
review_bytes=(G/'REVIEW.json').read_bytes();review=json.loads(review_bytes);target='eosio.'+kind
publication=json.loads((R/'GIT_REVIEW_PUBLISHED.json').read_text());assert publication['status']=='PASS'and publication['reviews'][kind]['sha256']==sha(review_bytes)
qual=R/review['qualified_attempt']/'RESULT.json';assert json.loads(qual.read_text())['status']=='PASS'and sha(qual.read_bytes())==review['qualification_SHA256']
assert review['chain_id']==CHAIN and review['target']==target and review['review_seconds']==1800
code=(G/'artifacts'/ (target+'.wasm')).read_bytes();abi=(G/'artifacts'/(target+'.raw.abi')).read_bytes();assert sha(code)==review['new_code_hash']and sha(abi)==review['new_abi_hash']
guard();state=rpc('get_upgrade_state');assert state['sealed']and state['review_seconds']==1800 and state['developer']=='eosio.dev'and target in state['protected_accounts']
for name,h in [('eosio','b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'),('eosio.token','f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c')]:assert rpc('get_code_hash',{'account_name':name})['code_hash']==h
if kind=='key':
 proof=json.loads((R/'governance/signup/EXECUTION.json').read_text());assert proof['status']=='UPGRADED_IRREVERSIBLE';assert rpc('get_code_hash',{'account_name':'eosio.signup'})['code_hash']==review['signup_code_hash']
if operation=='propose':
 assert not state['pending']and not(G/'PROPOSAL.json').exists()
else:
 proposal=json.loads((G/'PROPOSAL.json').read_text());assert proposal['status']=='PROPOSED_IRREVERSIBLE'and state['pending']and state['proposal_id']==proposal['governance']['proposal_id']and state['target']==target
 assert state['effective_voter_count']>0 and not state['execution_blocked_no_electorate']and state['effective_veto_count']<state['required_vetoes']
 lib=rpc('get_block',{'block_num_or_id':rpc('get_info')['last_irreversible_block_num']});assert datetime.datetime.fromisoformat(lib['timestamp'])>=datetime.datetime.fromisoformat(state['execute_after'])
if operation=='execute'and kind=='signup':assert int(rpc('get_account',{'account_name':target})['ram_quota'])>=int(review['code_RAM_target_quota_estimate'])
base=rpc('get_raw_abi',{'account_name':target});assert base['code_hash']==review['old_code_hash']and base['abi_hash']==review['old_abi_hash']
P=G/'private';P.mkdir(mode=0o700,exist_ok=True);W=Path('/opt')/('wsbgov-'+kind+'-20261105');W.mkdir(mode=0o700,exist_ok=True);cleos=Path('/opt/pureeos-keynet-20261103-r2/native-client/cleos');keosd=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/bin/keosd');socket_path=W/'keosd.sock';assert len(str(socket_path).encode())<108
class UnixHTTP(http.client.HTTPConnection):
 def connect(self):self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.sock.settimeout(10);self.sock.connect(str(socket_path))
def wallet_rpc(method,data):
 c=UnixHTTP('localhost');c.request('POST','/v1/wallet/'+method,json.dumps(data),{'Content-Type':'application/json'});r=c.getresponse();b=r.read();c.close();assert r.status==200,'Own developer wallet RPC refused';return json.loads(b)if b else None
def run(args,input_text=None):
 p=subprocess.run([str(cleos),'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(socket_path),*args],input=input_text,capture_output=True,text=True,timeout=90)
 
 if p.returncode:(P/'native-client-error.log').write_text(p.stdout+p.stderr)
 assert p.returncode==0,'Native client refusal (retained private log); no automatic new transaction'
 return p.stdout
def receipt(txid,start,timeout=600):
 deadline=time.monotonic()+timeout;next_block=start+1;previous=rpc('get_block',{'block_num_or_id':start})['id']
 while time.monotonic()<deadline:
  info=rpc('get_info')
  while next_block<=info['last_irreversible_block_num']:
   b=rpc('get_block',{'block_num_or_id':next_block});assert b['previous']==previous;previous=b['id']
   found=[t for t in b['transactions']if(t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==txid]
   if found:assert len(found)==1 and found[0]['status']=='executed';return {'block_num':b['block_num'],'block_id':b['id'],'timestamp':b['timestamp'],'transaction_id':txid,'irreversible_at_observed_LIB':info['last_irreversible_block_num']}
   next_block+=1
  time.sleep(2)
 raise RuntimeError('Confirmation timeout: reconcile immutable saved packet, do not re-sign or re-submit')
def send(label,tx):
 signed_path=P/(label+'-signed.json');progress_path=G/(label.upper()+'_PROGRESS.json');assert not signed_path.exists()and not progress_path.exists(),'Existing immutable packet MUST be reconciled; never repeat controller'
 unsigned=P/(label+'-unsigned.json');save(unsigned,tx)
 signed=json.loads(run(['push','transaction',str(unsigned),'-j','--expiration','1800','--dont-broadcast','--compression','zlib']));assert signed['signatures'];save(signed_path,signed)
 packed=json.loads(run(['convert','pack_transaction',str(signed_path)]));assert packed['compression']in[0,'none'];txid=sha(bytes.fromhex(packed['packed_trx']))
 before=guard();progress={'status':'SIGNED_IMMUTABLE','transaction_id':txid,'packet_sha256':sha(signed_path.read_bytes()),'start_LIB':before['last_irreversible_block_num'],'expiration':signed['expiration']};save(progress_path,progress)
 for attempt in range(1,4):
  guard();baseline=rpc('get_upgrade_state');prior=rpc('get_raw_abi',{'account_name':target});start=rpc('get_info')['last_irreversible_block_num'];progress['status']='BROADCASTING';save(progress_path,progress)
  p=subprocess.run([str(cleos),'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(socket_path),'push','transaction',str(signed_path),'-s','-j','--compression','zlib'],capture_output=True,text=True,timeout=90)
  (P/(label+'-response-'+str(attempt)+'.log')).write_text(p.stdout+p.stderr)
  try:value=json.loads(p.stdout)
  except Exception:raise RuntimeError('Ambiguous response: reconcile exact saved transaction before any further submission')
  assert value['transaction_id']==txid;trace=value['processed']
  if p.returncode==0:
   assert trace['receipt']['status']=='executed';progress.update(status='ACCEPTED_AWAITING_IRREVERSIBILITY',block_hint=trace['block_num']);save(progress_path,progress)
   result=receipt(txid,start);progress.update(status='IRREVERSIBLE',canonical_receipt=result);save(progress_path,progress);return result
  assert trace.get('receipt')is None and trace.get('except',{}).get('name')=='tx_cpu_usage_exceeded','Non-CPU rejection: no automatic retry'
  deadline=time.monotonic()+90
  while rpc('get_info')['last_irreversible_block_num']<trace['block_num']+2:assert time.monotonic()<deadline;time.sleep(2)
  end=rpc('get_info')['last_irreversible_block_num'];scanned=[]
  for n in range(start+1,end+1):
   b=rpc('get_block',{'block_num_or_id':n});assert not any((t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==txid for t in b['transactions']);scanned.append({'block':n,'id':b['id']})
  after=rpc('get_upgrade_state');assert all(after.get(k)==v for k,v in baseline.items()if k not in ['head_block_num','head_block_id']);assert rpc('get_raw_abi',{'account_name':target})['code_hash']==prior['code_hash']
  save(G/(label+'-CPU-absence-'+str(attempt)+'.json'),{'transaction_id':txid,'scanned':scanned,'unchanged_governance_and_code':True,'same_signed_bytes':True})
  progress['status']='CPU_REJECTED_CANONICAL_ABSENT';save(progress_path,progress);assert attempt<3,'Repeated CPU refusal; preserve original packet'
# Use ONLY the already-authorized developer role, never founder/BP/relay keys.
auth=rpc('get_account',{'account_name':'civicgovauth'})['permissions'];active=next(x['required_auth']for x in auth if x['perm_name']=='active');assert active['threshold']==1 and len(active['keys'])==1 and not active['accounts']and not active['waits']
devkey=json.loads(Path('/opt/pureeos-devnet-20261105-r1/private/bootstrap-keys.json').read_text())['accounts']['civicgovauth'];assert devkey['Public key']==active['keys'][0]['key']
with (P/'keosd.log').open('a')as log:
 child=subprocess.Popen([str(keosd),'--data-dir',str(W),'--config-dir',str(W),'--http-server-address',''],stdout=log,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+30
  while True:
   assert child.poll()is None and time.monotonic()<deadline,'Own developer wallet readiness timeout'
   try:wallet_rpc('list_wallets',[]);break
   except (OSError,http.client.HTTPException):time.sleep(.1)
  password=W/'password.txt'
  if not password.exists():run(['wallet','create','-n','bridge','--file',str(password)]);wallet_rpc('import_key',['bridge',devkey['Private key']])
  else:run(['wallet','open','-n','bridge']);wallet_rpc('unlock',['bridge',password.read_text().strip()])
  public_key=devkey['Public key'];devkey=None;keys=wallet_rpc('get_public_keys',[]);assert len(keys)==1 and public_key in run(['convert','public_key',keys[0]]),'Only the developer key may be loaded';wallet_rpc('set_timeout',7200)
  fields={k:review[k]for k in ['target','old_code_hash','old_abi_hash','new_code_hash','new_abi_hash']};fields['review_document_hash']=sha(review_bytes)
  if operation=='propose':
   prefix=[]
   if review.get('code_RAM_sponsor_action'):prefix=[review['code_RAM_sponsor_action']];assert kind=='signup'and prefix[0]['data']['payer']=='eosio.dev'and prefix[0]['data']['receiver']==target
   tx={'actions':prefix+[{'account':'eosio','name':name,'authorization':[{'actor':'eosio.dev','permission':'upgrade'}],'data':data}for name,data in [('refreshgov',{}),('devpropose',fields)]]};result=send('propose',tx);after=rpc('get_upgrade_state')
   assert after['pending']and after['target']==target and all(after[k]==v for k,v in fields.items());assert(datetime.datetime.fromisoformat(after['execute_after'])-datetime.datetime.fromisoformat(after['proposed_at'])).total_seconds()==1800
   save(G/'PROPOSAL.json',{'status':'PROPOSED_IRREVERSIBLE','chain_id':CHAIN,'target':target,'canonical_receipt':result,'proposal':fields,'governance':after,'review_seconds':1800})
   if prefix:
    account=rpc('get_account',{'account_name':target});save(G/'CODE_RAM_SPONSOR_RESULT.json',{'status':'PASS'if int(account['ram_quota'])>=int(review['code_RAM_target_quota_estimate'])else'NEEDS_REVIEW','action':prefix[0],'before_RAM':review['before_target_RAM'],'after_RAM':{k:account[k]for k in ['ram_quota','ram_usage']},'minimum_target_quota_estimate':review['code_RAM_target_quota_estimate'],'canonical_receipt':result,'same_atomic_proposal_transaction':True});assert int(account['ram_quota'])>=int(review['code_RAM_target_quota_estimate'])
  else:
   assert all(state[k]==v for k,v in fields.items());tx={'actions':[{'account':'eosio','name':'devexec','authorization':[{'actor':'eosio.dev','permission':'upgrade'}],'data':{'proposal_id':state['proposal_id'],'code':code.hex(),'abi':abi.hex()}}]};result=send('execute',tx);after=rpc('get_upgrade_state');actual=rpc('get_raw_abi',{'account_name':target});assert actual['code_hash']==sha(code)and actual['abi_hash']==sha(abi)and not after['pending']
   save(G/'EXECUTION.json',{'status':'UPGRADED_IRREVERSIBLE','chain_id':CHAIN,'target':target,'canonical_receipt':result,'new_code_hash':sha(code),'new_abi_hash':sha(abi),'review_seconds':1800,'before_governance':state,'after_governance':after})
  guard();print(json.dumps({'status':('PROPOSED_IRREVERSIBLE'if operation=='propose'else'UPGRADED_IRREVERSIBLE'),'target':target,'canonical_receipt':result,'execute_after':after['execute_after']}))
 finally:
  if child.poll()is None:child.terminate();child.wait(timeout=20)
