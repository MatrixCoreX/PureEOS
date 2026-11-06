"""Once-only new owned public functional payments; never historical fixtures."""
from pathlib import Path
from decimal import Decimal
import datetime,json,os,subprocess,time,traceback,urllib.request,urllib.error
import ops_common as o
from native_session import NativeSession
os.umask(0o077);R=o.R;NODE='/opt/pureeos-devnet-20261105-r1/tools/node-current/bin/node'
record={'status':'RUNNING','chain_id':o.CHAIN,'cases':[],'checks':[],'scope':'New owned keys and small existing test funds; no issuance, old fixture replay, TPS or phone-wallet test'}
def save():o.save(R/'PUBLIC_QA_PROGRESS.json',record)
def check(ok,label):
 assert ok,label;record['checks'].append(label);save();print('PASS '+label,flush=True)
def atoms(v):return int(Decimal(v.split()[0])*10000)
def amount(v):return f'{v//10000}.{v%10000:04d} AIC'
def table(n,scope='eosio.key'):return o.rows('eosio.key',scope,n)
def native(name,token='eosio.token',sym='AIC'):
 v=o.rpc('get_currency_balance',{'code':token,'account':name,'symbol':sym});return atoms(v[0])if v else 0
def pub(s):
 alphabet='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';v=0
 for c in(s[7:]if s.startswith('PUB_K1_')else s[3:]):v=v*58+alphabet.index(c)
 return v.to_bytes(37,'big')[:33]
def wallet(k):return next((w for w in table('wallets')if pub(w['key'])==pub(k['Public key'])),None)
def balance(k,token='eosio.token',sym='AIC'):
 a=next((x for x in table('assets')if x['token_contract']==token and x['token_symbol']=='4,'+sym),None);w=wallet(k)
 return next((int(x['amount'])for x in table('balances',str(w['id']))if int(x['asset_id'])==int(a['id'])),0)if a and w else 0
def reserves():
 for attempt in range(3):
  before=table('assets');sums={}
  for w in table('wallets'):
   for b in table('balances',str(w['id'])):sums[int(b['asset_id'])]=sums.get(int(b['asset_id']),0)+int(b['amount'])
  fees=table('feebuckets');nativefees=table('nativefees');actual={a['token_contract']:native('eosio.key',a['token_contract'],a['token_symbol'].split(',')[1])for a in before}
  if before!=table('assets'):time.sleep(.2);continue
  for a in before:
   n=sums.get(int(a['id']),0)
   if int(a['id'])==1:n+=sum(int(b['amount'])for b in nativefees)+sum(int(b['network'])+int(b['relay_earned'])for b in fees)
   assert n==int(a['reserve'])==actual[a['token_contract']],('Vault reserve/liability mismatch',a['token_symbol'])
   assert not a['busy']and int(a['ram_used'])<=int(a['ram_purchased'])
  assert not table('pending')and not nativefees;return
 raise RuntimeError('Concurrent ledger mutation: retry read-only, never repeat payments')
def bridge(p):
 v=subprocess.run([NODE,str(R/'controllers/qa_bridge.mjs')],input=json.dumps(p),capture_output=True,text=True,timeout=60)
 if v.returncode:(R/'public-qa-private/bridge-error.log').write_text(v.stderr)
 assert v.returncode==0,'Owned SDK failure; retained private log';return json.loads(v.stdout)
def http(path,data=None):
 req=urllib.request.Request('http://127.0.0.1:8787'+path,data=json.dumps(data).encode()if data is not None else None,headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=45)as r:return json.load(r)
def history(tid):
 deadline=time.monotonic()+150
 while True:
  try:
   with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+tid,timeout=20)as f:d=json.load(f)
   acts=d.get('actions',[])
   if any(a['act']['account']=='eosio.key'and a['act']['name']=='feereceipt'for a in acts):return acts
  except urllib.error.HTTPError as e:
   if e.code not in [404,503]:raise
  assert time.monotonic()<deadline,'Exact fee trace indexing incomplete';time.sleep(2)
def unique(acts,contract,name):
 found={}
 for a in acts:
  if a['act']['account']==contract and a['act']['name']==name:found[(a.get('global_sequence'),a['action_ordinal'])]=a['act']['data']
 return list(found.values())
def checked(label,call,budget):
 o.guard();o.require_codes(o.NEW);o.authority_guard();before=o.rpc('get_account',{'account_name':'eosio.key'});old={b['owner']:b for b in table('feebuckets')};growth=table('growth')
 result=call();acts=history(result['transaction_id']);fees=unique(acts,'eosio.key','feereceipt');assert len(fees)==1;fee=fees[0]
 buys=[x for x in unique(acts,'eosio','logbuyram')if x['payer']==x['receiver']=='eosio.key'];after=o.rpc('get_account',{'account_name':'eosio.key'});delta=int(after['ram_quota'])-int(before['ram_quota'])
 check(int(fee['allocated_bytes'])==budget,label+' allocation budget')
 check(atoms(fee['total'])==atoms(fee['ram_payment'])+atoms(fee['retained_base'])+atoms(fee['relay_fee'])and atoms(fee['total'])<=10000,label+' bounded exact fee equation')
 if budget:check(len(buys)==1 and atoms(buys[0]['quantity'])==atoms(fee['ram_payment'])and int(buys[0]['bytes'])==delta==int(fee['purchased_bytes'])and delta>=budget,label+' actual paid native RAM cost/quota')
 else:check(not buys and delta==0 and atoms(fee['ram_payment'])==0,label+' existing row purchases no KEY RAM')
 receipts=unique(acts,'eosio.key','receipt');assert len(receipts)==1;relay=receipts[0]['relayer'];owner=relay or'eosio.key';new={b['owner']:b for b in table('feebuckets')};a=old.get(owner,{'network':0,'relay_earned':0});b=new[owner]
 income=atoms(fee['retained_base'])+atoms(fee['relay_fee'])if relay else 0
 check(int(b['relay_earned'])-int(a['relay_earned'])==income and int(b['network'])-int(a['network'])==(0 if relay else atoms(fee['retained_base'])),label+' exact fee destination')
 entry={'label':label,**result,'fee':fee,'native_KEY_RAM':buys,'KEY_quota_delta':delta,'SDK_quote':result.get('quote'),'own_new_relay_income_atoms':income,'growth_before':growth,'growth_after':table('growth')};record['cases'].append(entry);save();reserves();return fee
try:
 assert not(R/'PUBLIC_QA_PROGRESS.json').exists(),'Existing QA requires canonical reconciliation; never rerun'
 assert json.loads((R/'CLIENTS_ACTIVATED.json').read_text())['status']=='PASS';o.guard();o.require_codes(o.NEW);o.authority_guard();reserves();save()
 with NativeSession()as session:
  keys={n:session.key()for n in ['A','B','C']};o.save(R/'public-qa-private/keys.json',keys);record['public_keys']={n:k['Public key']for n,k in keys.items()};save()
  A,B,C=[keys[n]for n in ['A','B','C']];named='keygrowcheck'
  try:o.rpc('get_account',{'account_name':named})
  except urllib.error.HTTPError as e:assert e.code==500
  else:raise AssertionError('Owned QA name exists; choose a fresh name before signing')
  cfg=table('config')[0];record['before_growth']=table('growth');record['legacy_counts_before']={k:cfg[k]for k in ['wallet_count','balance_count','asset_count']};save()
  def deposit(k,q,token='eosio.token'):
   req={'from':'gptgenesisbp','token_contract':token,'to':k['Public key'],'quantity':q,'max_fee':'1.0000 AIC','request_id':os.urandom(32).hex()};p=bridge({'mode':'deposit','request':req});assert len(p['actions'])==3
   return{**session.send('deposit-'+str(len(record['cases'])),p['actions']),'quote':p['quote']}
  def send(k,op,to,q,token='eosio.token',memo='Owned global-growth functional test'):
   req={'from':k['Public key'],'to':to,'token_contract':token,'quantity':q,'memo':memo,'auth_epoch':str(wallet(k)['auth_epoch']),'request_id':os.urandom(32).hex(),'relayer':'gptgenesisbp','reward_to':'gptgenesisbp','max_fee':'1.0000 AIC'}
   p=bridge({'mode':'prepare','operation':op,'request':req,'key':k['Private key']});label='relay-'+str(len(record['cases']));o.save(R/'public-qa-private'/(label+'-body.json'),p['body']);start=o.guard()['last_irreversible_block_num'];o.save(R/'public-qa-packets'/(label+'.json'),{'status':'OWNER_SIGNED_NOT_REPLAYABLE','request_id':req['request_id'],'body_SHA256':o.sha(json.dumps(p['body'],sort_keys=True).encode()),'start_LIB':start})
   reply=http('/submit',p['body']);o.save(R/'public-qa-packets'/(label+'-reply.json'),reply);receipt=o.receipt(reply['transaction_id'],start,timeout=180)
   deadline=time.monotonic()+90
   while True:
    status=http('/status/'+reply['intent'])
    if status['status']=='finalized':break
    assert time.monotonic()<deadline;time.sleep(2)
   assert status['confirmation']['block_id']==receipt['block_id'];time.sleep(3)
   return{'transaction_id':reply['transaction_id'],'canonical_receipt':receipt,'quote':p['quote']}
  for token,sym,budget in [('eosio.token','AIC',1536),('usd','USD',512),('cny','CNY',512)]:
   oldnative=native('gptgenesisbp');fee=checked('named-to-key '+sym,lambda:deposit(A,'2.0000 '+sym,token),budget)
   check(balance(A,token,sym)==20000,'Named-to-key full principal '+sym);check(oldnative-native('gptgenesisbp')==atoms(fee['total'])+(20000 if sym=='AIC'else 0),'Named sender pays additional fee '+sym)
  for token,sym,q,budget in [('eosio.token','AIC','0.5000 AIC',1536),('usd','USD','0.0100 USD',512),('cny','CNY','0.0100 CNY',512)]:
   checked('key-to-key '+sym,lambda:send(A,'ksend',B['Public key'],q,token),budget);check(balance(B,token,sym)==atoms(q),'Key-to-key full principal '+sym)
  before=balance(A);held=native('eosio.signup');fee=checked('atomic KEY-to-Signup',lambda:send(A,'withdraw','eosio.signup','0.2500 AIC',memo=named+'-'+C['Public key']),0)
  account=o.rpc('get_account',{'account_name':named});permissions={p['perm_name']:p['required_auth']for p in account['permissions']}
  check(all(a['threshold']==1 and len(a['keys'])==1 and pub(a['keys'][0]['key'])==pub(C['Public key'])and not a['accounts']and not a['waits']for a in permissions.values()),'Signup exact owned C authority, no dev delegation')
  check(int(account['ram_quota'])>=4096 and account['self_delegated_bandwidth']['net_weight']=='0.1000 AIC'and account['self_delegated_bandwidth']['cpu_weight']=='0.1000 AIC','Signup real >=4096 RAM and account-owned CPU/NET')
  check(before-balance(A)==2500+atoms(fee['total'])and native('eosio.signup')==held and not o.rows('eosio.signup','eosio.signup','pendingkey'),'Signup full payment, no retained funds or pending row')
  record['signup_account']=named;save()
  for token,sym,q in [('eosio.token','AIC','0.1000 AIC'),('usd','USD','0.0010 USD'),('cny','CNY','0.0010 CNY')]:
   checked('key-to-named '+sym,lambda:send(B,'withdraw',named,q,token),512);check(native(named,token,sym)==atoms(q),'Key-to-named full principal '+sym)
  checked('named-to-USD-only key',lambda:deposit(C,'0.1000 USD','usd'),1536);check(balance(C)==0 and balance(C,'usd','USD')==1000,'Receiving key needs no AIC; named/key holdings independent')
  earned=sum(c['own_new_relay_income_atoms']for c in record['cases']);old=next(b for b in table('feebuckets')if b['owner']=='gptgenesisbp');before=native('gptgenesisbp');assert earned>0 and int(old['relay_earned'])>=earned
  result=session.send('claim-only-own-new-income',[{'account':'eosio.key','name':'claim','authorization':[{'actor':'gptgenesisbp','permission':'active'}],'data':{'bp':'gptgenesisbp','to':'gptgenesisbp','quantity':amount(earned),'max_fee':'0.0000 AIC'}}]);new=next(b for b in table('feebuckets')if b['owner']=='gptgenesisbp')
  check(native('gptgenesisbp')-before==earned and int(old['relay_earned'])-int(new['relay_earned'])==earned and new['network']==old['network'],'Claim only new QA execution+relay earnings, fee zero and old reserve preserved')
  record['claim']={'atoms':earned,**result};reserves();growth=table('growth')[0];cfg=table('config')[0]
  check(all(int(growth[k])==int(cfg[k])for k in ['wallet_count','balance_count','asset_count']),'Exact growth matches compatible config at current scale')
  check(all(int(cfg['policy'][k])==1000 if k=='max_wallets'else int(cfg['policy'][k])==4000 if k=='max_balances'else True for k in ['max_wallets','max_balances']),'Old1000/4000 fields preserved without raising policy')
  record.update(status='PASS',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),after_growth=table('growth'),three_routes_three_assets=True,exact_reserves=True,no_issuance_or_old_payment_replay=True);o.guard();o.authority_guard();save();o.save(R/'PUBLIC_QA_RESULT.json',record)
except BaseException:
 record.update(status='NEEDS_CANONICAL_RECONCILIATION',error=traceback.format_exc());save();raise
