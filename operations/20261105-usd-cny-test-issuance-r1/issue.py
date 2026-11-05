"""Authorized USD/CNY cap changes and test allocation; not a repeatable fixture."""
from pathlib import Path
import json,os,sys,hashlib,datetime,time,subprocess,socket,urllib.request,urllib.error,traceback
os.umask(0o077)
C=Path('/opt/pureeos-devnet-20261105-r1');R=Path('/opt/pureeos-usd-cny-test-issuance-20261105-r1');P=R/'private'
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
TOKEN='ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb'
RAW_ABI='a64141f32a82aaa3185dad05678ef44b42a8f7a9a8dbc266fd2ee98fd2ffc90e'
cleos=str(C/'tools-runtime/programs/cleos/cleos');unit='pureeos-devnet-wallet-20261105.service'
CAP='1000000000.0000';QUANTITY='100000000.0000';EXPECTED='100010000.0000';RECIPIENT='gptgenesisbp'
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(n,v):
 p=R/n;t=p.with_suffix(p.suffix+'.tmp');t.write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n');os.replace(t,p)
def rpc(method,data={},port=8888):
 q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+method,json.dumps(data).encode(),{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=90 if method=='push_transaction'else 20)as f:return json.load(f)
def run(args,input_text=None):
 q=subprocess.run([cleos,'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(C/'founder/wallet/keosd.sock'),*args],input=input_text,capture_output=True,text=True,timeout=90)
 if q.returncode:
  # The password is never a command argument and no raw command/error is exported.
  message=(q.stdout+q.stderr).replace(password,'[REDACTED]')if 'password'in globals()else'Native client failed'
  raise RuntimeError(message[:4000])
 return json.loads(q.stdout)
def atoms(value):return int(value.split()[0].replace('.',''))
def balance(token,account):
 values=rpc('get_currency_balance',{'code':token,'account':account,'symbol':token.upper()});return atoms(values[0])if values else 0
def permissions(name):return rpc('get_account',{'account_name':name})['permissions']
def stats(token):return rpc('get_currency_stats',{'code':token,'symbol':token.upper()})[token.upper()]
def governance():return {k:v for k,v in rpc('get_upgrade_state').items()if k not in ['head_block_num','head_block_id']}
def table(name,scope='eosio.key'):
 rows=[];lower=''
 for page in range(100):
  v=rpc('get_table_rows',{'code':'eosio.key','scope':scope,'table':name,'json':True,'limit':1000,'lower_bound':lower});rows+=v['rows']
  if not v.get('more'):return rows
  new=str(v.get('next_key',''));assert new and new!=lower;lower=new
 raise AssertionError('Incomplete table')
def key_snapshot():
 wallets=table('wallets');return {'config':table('config'),'assets':table('assets'),'wallets':wallets,'balances':{str(w['id']):table('balances',str(w['id']))for w in wallets},'buckets':table('feebuckets'),'nativefees':table('nativefees'),'pending':table('pending'),'quota':rpc('get_account',{'account_name':'eosio.key'})['ram_quota']}
def token_snapshot():
 return {n:{'stats':stats(n),'raw_ABI':rpc('get_raw_abi',{'account_name':n})['abi_hash'],'code_hash':rpc('get_code_hash',{'account_name':n})['code_hash'],'permissions':permissions(n),'issuer_balance_atoms':balance(n,n),'recipient_balance_atoms':balance(n,RECIPIENT),'KEY_balance_atoms':balance(n,'eosio.key')}for n in ['usd','cny']}
def pids():return {u:int(subprocess.check_output(['systemctl','show',u,'-p','MainPID','--value'],text=True))for u in baseline_pids}
def absent(tid,start,end):
 blocks=[]
 for n in range(start,end+1):
  b=rpc('get_block',{'block_num_or_id':n});assert not any((x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==tid for x in b['transactions'])
  blocks.append({'block_num':n,'block_id':b['id']})
 return blocks
progress={'status':'PREFLIGHT','UTC':now(),'chain_id':CHAIN,'recipient':RECIPIENT,'cap_each':CAP,'additional_issuance_each':QUANTITY}
wallet_started=False
try:
 assert not(R/'PROGRESS.json').exists(),'Reconcile existing immutable packet; never repeat issuance'
 P.mkdir(mode=0o700,exist_ok=True);save('PROGRESS.json',progress)
 for port in [8888,8890]:assert rpc('get_info',port=port)['chain_id']==CHAIN
 features=rpc('get_activated_protocol_features',{'limit':500})['activated_protocol_features']
 assert any(v['feature_digest']=='8ba52fe7a3956c5cd3a656a3174b931d3bb2abb45578befc59f283ecd816a405'for v in features),'Existing first-authorizer billing required'
 resources=rpc('get_account',{'account_name':'eosio.dev'});assert int(resources['cpu_limit']['available'])>0 and int(resources['net_limit']['available'])>0
 baseline_pids=json.loads(Path('/opt/pureeos-relay-execution-income-20261105-r1/BEFORE.json').read_text())['node_pids'];assert len(baseline_pids)==25 and pids()==baseline_pids
 before=token_snapshot();aic=rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'});key=key_snapshot();gov=governance()
 assert aic['AIC']['supply']=='100000000.0000 AIC'and aic['AIC']['max_supply']=='2100000000.0000 AIC'
 assert not key['pending']and not key['nativefees']and not gov['pending']
 for n,v in before.items():
  sym=n.upper();assert v['code_hash']==TOKEN and v['raw_ABI']==RAW_ABI and v['stats']=={'supply':'10000.0000 '+sym,'max_supply':'1000000.0000 '+sym,'issuer':n}
  active=next(p['required_auth']for p in v['permissions']if p['perm_name']=='active')
  assert active['threshold']==1 and not active['keys']and not active['waits']and { (a['permission']['actor'],a['permission']['permission'],a['weight'])for a in active['accounts']}=={('eosio.dev','active',1),(n,'eosio.code',1)}
 dev=permissions('eosio.dev');assert next(p['required_auth']for p in dev if p['perm_name']=='active')=={'threshold':1,'keys':[],'accounts':[{'permission':{'actor':'civicgovlead','permission':'active'},'weight':1}],'waits':[]}
 actions=[]
 for n in ['usd','cny']:
  sym=n.upper();auth=[{'actor':n,'permission':'active'}]
  for name,data in [('setmaxsupply',{'issuer':n,'maximum_supply':CAP+' '+sym}),('issue',{'to':n,'quantity':QUANTITY+' '+sym,'memo':'User-authorized TEST transfer allocation; no fiat backing claim'}),('transfer',{'from':n,'to':RECIPIENT,'quantity':QUANTITY+' '+sym,'memo':'TEST transfer funding; 100 million additional '+sym})]:
   actions.append({'account':n,'name':name,'authorization':auth,'data':data})
 # Explicitly use the existing dev resources under the already-active billing
 # feature. Issuer authority is still required for every token action.
 actions[0]['authorization']=[{'actor':'eosio.dev','permission':'active'},{'actor':'usd','permission':'active'}]
 save('BEFORE.json',{'UTC':now(),'chain_id':CHAIN,'tokens':before,'AIC_stats':aic,'KEY_snapshot':key,'developer_permissions':dev,'node_pids':baseline_pids,'governance':gov})
 save('INTENT.json',{'chain_id':CHAIN,'actions':actions,'recipient':RECIPIENT,'expected_supply_each':EXPECTED,'native_CPU_NET_payer':'eosio.dev','existing_first_authorizer_feature':True,'scope':'Latest user cap10yi and additional issue1yi for EACH token. Single atomic six-root-action transaction; no contract/authority/AIC changes.'})
 was_active=subprocess.run(['systemctl','is-active','--quiet',unit]).returncode==0
 assert not was_active,'Unexpected existing signer session; preserve it and reconcile ownership'
 subprocess.run(['systemctl','start',unit],check=True);wallet_started=True
 deadline=time.monotonic()+60
 while True:
  s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
  try:s.settimeout(1);s.connect(str(C/'founder/wallet/keosd.sock'));break
  except OSError:assert time.monotonic()<deadline;time.sleep(.25)
  finally:s.close()
 password=(C/'founder/wallet/password.txt').read_text().strip()
 # Native wallet unlock writes only its public response to this process.
 q=subprocess.run([cleos,'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(C/'founder/wallet/keosd.sock'),'wallet','unlock','-n','localtest'],input=password+'\n',capture_output=True,text=True,timeout=30)
 assert q.returncode==0 or 'already unlocked'in(q.stdout+q.stderr).lower(),'Existing signer unlock failed'
 unsigned=P/'unsigned.json';unsigned.write_text(json.dumps({'actions':actions}))
 signed=run(['push','transaction',str(unsigned),'-j','-x','1800','--dont-broadcast','--compression','zlib']);assert signed['signatures']and len(signed['actions'])==6
 signed_path=P/'signed.json';signed_path.write_text(json.dumps(signed,indent=2)+'\n')
 packed=run(['convert','pack_transaction',str(signed_path)]);assert packed['compression']in[0,'none']
 tid=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest();public_keys=run(['wallet','keys'])
 required=rpc('get_required_keys',{'transaction':{k:v for k,v in signed.items()if k not in ['signatures','context_free_data']},'available_keys':public_keys})['required_keys']
 lead=permissions('civicgovlead');lead_keys={k['key']for p in lead if p['perm_name']in['owner','active']for k in p['required_auth']['keys']}
 assert required and set(required)<=lead_keys,'Issuer must resolve to the delegated dev holder, not BP authority'
 start=rpc('get_info');start_lib=start['last_irreversible_block_num']
 progress.update(status='SIGNED_IMMUTABLE',transaction_id=tid,start_LIB=start_lib,packet_SHA256=hashlib.sha256(signed_path.read_bytes()).hexdigest(),expiration=signed['expiration'],required_key_count=len(required),attempts=[]);save('PROGRESS.json',progress)
 accepted=None
 for attempt in range(1,4):
  progress['status']='BROADCASTING';save('PROGRESS.json',progress)
  try:
   result=rpc('push_transaction',packed);(P/('response-'+str(attempt)+'.json')).write_text(json.dumps(result))
   assert result['transaction_id']==tid and result['processed']['receipt']['status']=='executed'
   accepted=result;progress.update(status='ACCEPTED_AWAITING_IRREVERSIBILITY',reported_block=result['processed']['block_num']);save('PROGRESS.json',progress);break
  except urllib.error.HTTPError as e:
   body=e.read().decode();error=json.loads(body);save('REJECTION_'+str(attempt)+'.json',{'status':'REJECTED','transaction_id':tid,'HTTP':e.code,'error':error,'UTC':now()})
   assert error.get('error',{}).get('name')=='tx_cpu_usage_exceeded','Non-CPU refusal; reconcile before retry'
   deadline=time.monotonic()+120;target=rpc('get_info')['head_block_num']+2
   while rpc('get_info')['last_irreversible_block_num']<target:
    assert time.monotonic()<deadline,'CPU rejection canonical-absence wait timeout';time.sleep(1)
   end=rpc('get_info')['last_irreversible_block_num'];blocks=absent(tid,start_lib+1,end)
   assert token_snapshot()==before and key_snapshot()==key,'State changed after refusal; no automatic replay'
   progress['attempts'].append({'attempt':attempt,'type':'CPU_REJECTED_CANONICAL_ABSENT','blocks':blocks,'same_packet':True});progress['status']='CPU_REJECTED_CANONICAL_ABSENT';save('PROGRESS.json',progress)
   assert attempt<3,'Keep original packet; bounded CPU retries exhausted'
  except (OSError,urllib.error.URLError,TimeoutError):
   progress['status']='AMBIGUOUS_AWAITING_CANONICAL_RECONCILIATION';save('PROGRESS.json',progress);break
 deadline=time.monotonic()+600;next_block=start_lib+1;canonical=None
 while time.monotonic()<deadline and canonical is None:
  lib=rpc('get_info')['last_irreversible_block_num']
  while next_block<=lib:
   b=rpc('get_block',{'block_num_or_id':next_block})
   matches=[x for x in b['transactions']if(x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==tid]
   if matches:
    assert len(matches)==1 and matches[0]['status']=='executed';canonical={'transaction_id':tid,'block_num':next_block,'block_id':b['id'],'observed_LIB':lib,'CPU_us':matches[0]['cpu_usage_us'],'NET_words':matches[0]['net_usage_words']};break
   next_block+=1
  if canonical:break
  time.sleep(1)
 assert canonical,'Do not resend signed issuance; reconcile exact ID and expiration'
 progress.update(status='IRREVERSIBLE',canonical_receipt=canonical);save('PROGRESS.json',progress)
 after=token_snapshot()
 for n,v in after.items():
  sym=n.upper();assert v['stats']=={'supply':EXPECTED+' '+sym,'max_supply':CAP+' '+sym,'issuer':n}
  for name in ['raw_ABI','code_hash','permissions','issuer_balance_atoms','KEY_balance_atoms']:assert v[name]==before[n][name],(n,name)
  assert v['recipient_balance_atoms']-before[n]['recipient_balance_atoms']==1000000000000
 assert rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})==aic and key_snapshot()==key and permissions('eosio.dev')==dev and pids()==baseline_pids
 assert governance()==gov
 # Wait for actual indexed root-action data, without changing or resetting indices.
 roots=None;deadline=time.monotonic()+120
 while time.monotonic()<deadline:
  try:
   with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+tid,timeout=20)as h:history=json.load(h)
   entries=history.get('actions',[])
   selected=[x for x in entries if x['receiver']==x['act']['account']and x.get('creator_action_ordinal',0)==0]
   if len(selected)==6:
    assert all(x['block_num']==canonical['block_num']and x['block_id']==canonical['block_id']for x in entries)
    roots=[{'account':x['act']['account'],'name':x['act']['name'],'authorization':x['act']['authorization'],'data':x['act']['data']}for x in sorted(selected,key=lambda x:x['action_ordinal'])]
    assert roots==actions,'Canonical root actions must equal exact reviewed intent';break
  except urllib.error.HTTPError as e:
   assert e.code in[404,503]
  time.sleep(1)
 assert roots==actions,'Canonical transaction confirmed; reconcile history separately, do not repeat mint'
 result={'status':'PASS','UTC':now(),'chain_id':CHAIN,'transaction_id':tid,'canonical_receipt':canonical,'actions':actions,'before':before,'after':after,'additional_issued_each':'100000000.0000','cap_each':'1000000000.0000','issued_supply_each':'100010000.0000','recipient':RECIPIENT,'native_CPU_NET_payer':'eosio.dev','original25_node_PIDs_unchanged':True,'AIC_stats_unchanged':aic,'KEY_ledger_reserves_RAM_unchanged':True,'code_ABI_authorities_unchanged':True,'developer_authority_used':True,'single_atomic_transaction':True,'no_governance_proposal':True,'native_history_actions_match':True,'signing_packet_exported':False,'scope':'Authorized issuer operations only. No AIC issuance/cap changes, node stop/reset, votes/bonds, contract upgrade, new server or TPS. No earlier successful payment replay.'}
 save('RESULT.json',result);progress['status']='PASS';save('PROGRESS.json',progress);print('USD_CNY_CAP_1B_ADDITIONAL_100M_EACH_IRREVERSIBLE '+tid,flush=True)
except BaseException:
 progress.update(status='NEEDS_CANONICAL_RECONCILIATION',error=traceback.format_exc().replace(password,'[REDACTED]')if 'password'in globals()else traceback.format_exc());save('PROGRESS.json',progress);raise
finally:
 if wallet_started:
  subprocess.run(['systemctl','stop',unit],check=True)
  save('TEMP_SIGNER_CLOSED.json',{'status':'PASS','UTC':now(),'stopped_only_owned_temporary_signer':unit,'active':subprocess.run(['systemctl','is-active','--quiet',unit]).returncode==0})
