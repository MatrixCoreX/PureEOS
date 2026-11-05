"""Bounded actual public-chain functional QA, no bootstrap, fake metrics or old payment replay."""
from pathlib import Path
import datetime,hashlib,json,os,subprocess,time,traceback,urllib.request,urllib.error
from decimal import Decimal
from campaign_helpers import R,load_chain,permission,action
f=load_chain();assert json.loads((R/'KEY_SERVICE_RESULT.json').read_text())['status']=='PASS'
assert not(R/'PUBLIC_TRANSFER_PROGRESS.json').exists(),'Reconcile existing signed IDs and all balances before resuming'
assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']=='7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788'
ordinary={v['name']for v in json.loads((R/'voting/PLAN.json').read_text())['voters']}
assert len(ordinary)==90 and ordinary.isdisjoint({'gptgenesisbp','chainnamedai','eosio.key','eosio.signup','usd','cny'})
assert hashlib.sha256((R/'activity/sender.py').read_bytes()).hexdigest()==json.loads((R/'activity/QUALIFIED.json').read_text())['sender_SHA256']
record={'status':'RUNNING','chain_id':f.rpc('get_info')['chain_id'],'cases':[],'checks':[],'scope':'Owned demo keys and bounded functional cases only, no TPS benchmark; ordinary90 keeps running with separate accounts/wallet, transfers only among its own voters and civiccounter calls, no KEY/issuer/founder balance or RAM mutations','ordinary_business_retained':True}
def save():(R/'PUBLIC_TRANSFER_PROGRESS.json').write_text(json.dumps(record,indent=2)+'\n')
def check(ok,label):
 assert ok,label;record['checks'].append(label);save();print('PASS '+label,flush=True)
def amount(n,symbol='AIC'):return f'{n//10000}.{n%10000:04d} {symbol}'
def atoms(s):return int(Decimal(s.split()[0])*10000)
def table(n,scope='eosio.key'):return f.table(n,code='eosio.key',scope=scope)
def native(name,token='eosio.token',sym='AIC'):
 row=f.rpc('get_currency_balance',{'code':token,'account':name,'symbol':sym});return atoms(row[0])if row else 0
def pub(s):
 alphabet='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';v=0
 for c in(s[7:]if s.startswith('PUB_K1_')else s[3:]):v=v*58+alphabet.index(c)
 return v.to_bytes(37,'big')[:33]
def wallet(k):return next((w for w in table('wallets')if pub(w['key'])==pub(k['Public key'])),None)
def balance(k,token='eosio.token',sym='AIC'):
 a=next((x for x in table('assets')if x['token_contract']==token and x['token_symbol']=='4,'+sym),None);w=wallet(k)
 return next((int(x['amount'])for x in table('balances',str(w['id']))if int(x['asset_id'])==int(a['id'])),0)if a and w else 0
def reserves():
 sums={}
 for w in table('wallets'):
  for b in table('balances',str(w['id'])):sums[int(b['asset_id'])]=sums.get(int(b['asset_id']),0)+int(b['amount'])
 for a in table('assets'):
  n=sums.get(int(a['id']),0)
  if int(a['id'])==1:n+=sum(int(b['amount'])for b in table('nativefees'))+sum(int(b['network'])+int(b['relay_earned'])for b in table('feebuckets'))
  sym=a['token_symbol'].split(',')[1];assert n==int(a['reserve'])==native('eosio.key',a['token_contract'],sym),'Exact native reserve/liability mismatch '+sym
 assert not table('pending')and not table('nativefees')
def bridge(p):
 q=subprocess.run([str(R/'tools/node-current/bin/node'),str(R/'qa_bridge.mjs')],input=json.dumps(p),capture_output=True,text=True,timeout=45)
 assert not q.returncode,f.redact(q.stderr);return json.loads(q.stdout)
def http(path,data=None):
 req=urllib.request.Request('http://127.0.0.1:8787'+path,data=json.dumps(data).encode()if data is not None else None,headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=35)as r:return json.load(r)
def history(txid):
 deadline=time.monotonic()+90
 while True:
  try:
   with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+txid,timeout=15)as r:data=json.load(r)
   acts=data.get('actions',[])
   if any(a['act']['account']=='eosio.key'and a['act']['name']=='feereceipt'for a in acts):return acts
  except urllib.error.HTTPError as e:
   if e.code not in[404,503]:raise
  assert time.monotonic()<deadline,'Exact fee trace indexing timeout';time.sleep(2)
def unique(acts,contract,name):
 found={}
 for a in acts:
  if a['act']['account']==contract and a['act']['name']==name:found[(a.get('global_sequence'),a['action_ordinal'])]=a['act']['data']
 return list(found.values())
def checked(label,call,budget):
 before=f.rpc('get_account',{'account_name':'eosio.key'});result=call();txid=result['transaction_id'];acts=history(txid)
 fees=unique(acts,'eosio.key','feereceipt');assert len(fees)==1;fee=fees[0];buys=unique(acts,'eosio','logbuyram');after=f.rpc('get_account',{'account_name':'eosio.key'});quota=int(after['ram_quota'])-int(before['ram_quota'])
 check(int(fee['allocated_bytes'])==budget,label+' conservative byte budget')
 check(atoms(fee['total'])==atoms(fee['ram_payment'])+atoms(fee['retained_base'])+atoms(fee['relay_fee']),label+' exact fee equation')
 if budget:
  check(len(buys)==1 and buys[0]['payer']==buys[0]['receiver']=='eosio.key'and atoms(buys[0]['quantity'])==atoms(fee['ram_payment'])and int(buys[0]['bytes'])==quota==int(fee['purchased_bytes'])and quota>=budget,label+' actual native RAM quota/cost')
 else:check(not buys and quota==0 and atoms(fee['ram_payment'])==0,label+' existing row buys no RAM')
 reserves();entry={'label':label,'transaction_id':txid,'canonical_receipt':result['canonical_receipt'],'fee':fee,'native_RAM_purchase':buys,'quota_delta':quota,'SDK_quote':result.get('quote')};record['cases'].append(entry);save();return fee
def deposit(k,q,token='eosio.token'):
 req={'from':'gptgenesisbp','token_contract':token,'to':k['Public key'],'quantity':q,'max_fee':'1.0000 AIC','request_id':os.urandom(32).hex()}
 prepared=bridge({'mode':'deposit','request':req});a=prepared['actions'];assert len(a)==3 and all(x['authorization']==[{'actor':'gptgenesisbp','permission':'active'}]for x in a)
 result=f.self_transact('owned-demo-atomic-deposit',a);return{'transaction_id':result['transaction_id'],'canonical_receipt':f.report['confirmed_transactions'][-1],'quote':prepared['quote']}
def send(k,op,to,q,token='eosio.token',cap='1.0000 AIC'):
 req={'from':k['Public key'],'to':to,'token_contract':token,'quantity':q,'memo':'Owned TEST atomic RAM functional QA','auth_epoch':str(wallet(k)['auth_epoch']),'request_id':os.urandom(32).hex(),'relayer':'gptgenesisbp','reward_to':'gptgenesisbp','max_fee':cap}
 prepared=bridge({'mode':'prepare','operation':op,'request':req,'key':k['Private key']});start=f.rpc('get_info')['last_irreversible_block_num'];reply=http('/submit',prepared['body']);confirmation=f.wait_transaction(reply['transaction_id'],start,timeout=120)
 deadline=time.monotonic()+60
 while True:
  status=http('/status/'+reply['intent'])
  if status['status']=='finalized':break
  assert time.monotonic()<deadline;time.sleep(3)
 assert status['confirmation']['block_id']==confirmation['block_id'];again=http('/submit',prepared['body']);assert again['transaction_id']==reply['transaction_id']and again['status']=='finalized'
 # Keep well below the actual HTTP rate limit; status/replay checks are functional, not a workload.
 time.sleep(3)
 return{'transaction_id':reply['transaction_id'],'canonical_receipt':confirmation,'quote':prepared['quote']}
def stable():
 return{'wallets':table('wallets'),'assets':table('assets'),'balances':{str(w['id']):table('balances',str(w['id']))for w in table('wallets')},'fees':table('feebuckets'),'nativefees':table('nativefees'),'AIC':native('eosio.key'),'USD':native('eosio.key','usd','USD'),'CNY':native('eosio.key','cny','CNY'),'quota':f.rpc('get_account',{'account_name':'eosio.key'})['ram_quota']}
try:
 save();keys={n:f.key()for n in ['A','B','USDonly','named']};(R/'private/public-owned-demo-keys.json').write_text(json.dumps(keys));(R/'private/public-owned-demo-keys.json').chmod(0o600)
 named='chainnamedai';auth=permission(keys['named']['Public key']);f.self_transact('create-owned-demo-unopened-recipient',[action('newaccount','gptgenesisbp',{'creator':'gptgenesisbp','name':named,'owner':auth,'active':auth}),action('buyram','gptgenesisbp',{'payer':'gptgenesisbp','receiver':named,'quant':'0.0200 AIC'})])
 record['public_keys']={k:v['Public key']for k,v in keys.items()};record['named_recipient']=named;record['bootstrap_RAM']={k:f.rpc('get_account',{'account_name':'eosio.key'})[k]for k in ['ram_quota','ram_usage']};save()
 A,B=keys['A'],keys['B']
 for token,sym,q,budget in [('usd','USD','100.0000 USD',3584),('eosio.token','AIC','20.0000 AIC',512),('cny','CNY','100.0000 CNY',2560)]:
  before=native('gptgenesisbp');fee=checked('named-to-key first '+sym,lambda:deposit(A,q,token),budget)
  check(balance(A,token,sym)==atoms(q),'Full named-to-key principal '+sym)
  check(before-native('gptgenesisbp')==atoms(fee['total'])+(atoms(q)if sym=='AIC'else 0),'Named sender additional actual AIC fee '+sym)
 for token,sym,q,budget in [('eosio.token','AIC','3.0000 AIC',1536),('usd','USD','2.0000 USD',512),('cny','CNY','2.0000 CNY',512),('eosio.token','AIC','1.0000 AIC',0)]:
  before=balance(B,token,sym);checked('key-to-key '+sym+(' existing'if not budget else' new'),lambda:send(A,'ksend',B['Public key'],q,token),budget);check(balance(B,token,sym)-before==atoms(q),'Full key-to-key principal '+sym)
 for token,sym,q,budget in [('eosio.token','AIC','1.0000 AIC',512),('usd','USD','1.0000 USD',512),('cny','CNY','1.0000 CNY',512),('usd','USD','0.5000 USD',0)]:
  before=native(named,token,sym);checked('key-to-named '+sym+(' existing'if not budget else' unopened'),lambda:send(A,'withdraw',named,q,token),budget);check(native(named,token,sym)-before==atoms(q),'Full key-to-named principal '+sym)
 checked('named-to-USD-only key',lambda:deposit(keys['USDonly'],'2.0000 USD','usd'),1536);check(balance(keys['USDonly'])==0,'USD recipient needs no AIC')
 before=stable()
 try:send(keys['USDonly'],'ksend',B['Public key'],'1.0000 USD','usd')
 except urllib.error.HTTPError as e:assert e.code==400 and'insufficient additional aic'in e.read().decode().lower()
 else:raise AssertionError('Sender without AIC was accepted')
 check(stable()==before,'USD sender without AIC rejected without ledger/RAM/reserve mutation')
 before=stable();req={'from':A['Public key'],'to':f.key()['Public key'],'token_contract':'eosio.token','quantity':'0.1000 AIC','memo':'Signed zero-cap rollback QA','auth_epoch':str(wallet(A)['auth_epoch']),'request_id':os.urandom(32).hex(),'relayer':'gptgenesisbp','reward_to':'gptgenesisbp','max_fee':'0.0000 AIC'}
 hot=json.loads((R/'private/relay-key.json').read_text());packet=bridge({'mode':'capfailure','operation':'ksend','request':req,'key':A['Private key'],'hot_key':hot['Private key']})
 (R/'founder/signed-zero-cap-packet.json').write_text(json.dumps(packet))
 try:f.rpc('push_transaction',packet['packed'])
 except RuntimeError as e:assert 'fee exceeds signed maximum'in str(e).lower(),f.redact(str(e))
 else:raise AssertionError('Native signed zero cap was accepted')
 check(stable()==before,'Actual native signed fee-cap rejection atomically preserves ledger, quota and reserves')
 record['native_cap_rejection']={'transaction_id':packet['id'],'status':'native rejection; no successful canonical receipt','packet_SHA256':hashlib.sha256((R/'founder/signed-zero-cap-packet.json').read_bytes()).hexdigest()};save()
 before=stable();f.tx(['transfer','gptgenesisbp','eosio.key','0.0100 AIC','fee'],failure='use atomic deposit');check(stable()==before,'Legacy standalone fee transfer atomically rejected')
 earned=next(int(b['relay_earned'])for b in table('feebuckets')if b['owner']=='gptgenesisbp');network=sum(int(b['network'])for b in table('feebuckets'));before=native('gptgenesisbp');assert earned>0
 result=f.action('claim',{'bp':'gptgenesisbp','to':'gptgenesisbp','quantity':amount(earned),'max_fee':'0.0000 AIC'},'gptgenesisbp',contract='eosio.key')
 check(native('gptgenesisbp')-before==earned and next(int(b['relay_earned'])for b in table('feebuckets')if b['owner']=='gptgenesisbp')==0 and sum(int(b['network'])for b in table('feebuckets'))==network,'Full free relay-income claim; network reserve remains unclaimed')
 record['free_claim']={'atoms':earned,'canonical_receipt':f.report['confirmed_transactions'][-1]};reserves();record.update(status='PASS',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),nativefee_credit_rows=0,reserves_exact=True,all_three_routes_all_three_tokens=True,HTTP_replay_same_native_ID=True)
 save();(R/'PUBLIC_TRANSFER_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print('PUBLIC_THREE_ROUTES_THREE_ASSETS_ATOMIC_RAM_PASS',flush=True)
except BaseException:
 record.update(status='NEEDS_CANONICAL_RECONCILIATION',error=f.redact(traceback.format_exc()));save();raise
