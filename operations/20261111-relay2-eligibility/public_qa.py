"""Fresh relay2 public HTTPS functional QA, new private test keys only.
One0.5000AIC native funding and two0.0001AIC owner-authorized transfers.
Never replays original tasks or signs from phone wallets.
"""
from pathlib import Path
import json,subprocess,urllib.request,urllib.error,datetime,hashlib,time,os
import ops_common as o
from native_session import NativeSession
R=o.R;S=R/'relay2-runtime';P=R/'public-qa-private';BP='anthropiclab';endpoint='https://relay2.matrixai.one';os.umask(0o077)
assert not(R/'PUBLIC_QA_STARTED.json').exists();assert json.loads((R/'RELAY2_SERVICE_RESULT.json').read_text())['status']=='PASS_NEW_SERVICE_AND_PUBLIC_HTTPS_HEALTH';o.guard();o.require_codes(o.NEW);o.save(R/'PUBLIC_QA_STARTED.json',{'scope':__doc__,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat()})
# Retry only transient READ requests; never repeat owner signing or HTTP submission.
base_rpc=o.rpc
def read_rpc(method,data=None,port=8888):
 for n in range(5):
  try:return base_rpc(method,data,port)
  except urllib.error.HTTPError as e:
   raw=e.read().decode(errors='replace');(R/'read-rpc-retries.log').open('a').write(json.dumps({'method':method,'HTTP':e.code,'body':raw,'attempt':n+1})+'\n')
   if e.code not in[500,502,503,504]or n==4:raise
   time.sleep(.2*2**n)
o.rpc=read_rpc
result={'status':'IN_PROGRESS','cases':[]};save=lambda:o.save(R/'PUBLIC_QA_RESULT.json',result);save()
def node(code,data):
 p=subprocess.run(['/opt/pureeos-devnet-20261105-r1/tools/node-current/bin/node','--max-old-space-size=96','--input-type=module','-e',code],input=json.dumps(data),capture_output=True,text=True,cwd=S,timeout=45)
 if p.returncode:(P/'node-error.log').write_text(p.stderr)
 assert p.returncode==0,'Owned SDK preparation refused; preserve private log';return json.loads(p.stdout)
def keywallet(pub):
 digest=node("import{keyHash}from './sdk/index.mjs';let s='';for await(const c of process.stdin)s+=c;console.log(JSON.stringify(keyHash(JSON.parse(s).key)));",{'key':pub})
 return next(w for w in o.rows('eosio.key','eosio.key','wallets')if w['key_hash']==digest)
def balance(pub):return next(int(b['amount'])for b in o.rows('eosio.key',str(keywallet(pub)['id']),'balances')if int(b['asset_id'])==1)
def atoms(v):return int(__import__('decimal').Decimal(v.split()[0])*10000)
def fetch(path):
 q=urllib.request.Request(path,headers={'User-Agent':'PureEOS-relay2-functional-QA/1.0'})
 with urllib.request.urlopen(q,timeout=30)as f:return json.load(f)
with NativeSession()as signer:
 owner=signer.key();receiver=signer.key();o.save(P/'keys.json',{'owner':owner,'receiver':receiver});public=node("import{normalizePublicKey}from './sdk/index.mjs';let s='';for await(const c of process.stdin)s+=c;const d=JSON.parse(s);console.log(JSON.stringify({owner:normalizePublicKey(d.owner),receiver:normalizePublicKey(d.receiver)}));",{'owner':owner['Public key'],'receiver':receiver['Public key']})
 auth=[{'actor':'gptgenesisbp','permission':'active'}];actions=[{'account':'eosio.key','name':'deposit','authorization':auth,'data':{'from':'gptgenesisbp','token_contract':'eosio.token','to':public['owner'],'quantity':'0.5000 AIC','max_fee':'0.0000 AIC','request_id':hashlib.sha256(b'relay2-case-funding-20261111').hexdigest()}},{'account':'eosio.token','name':'transfer','authorization':auth,'data':{'from':'gptgenesisbp','to':'eosio.key','quantity':'0.5000 AIC','memo':'atomic key principal'}}]
 funding=signer.send('funding',actions);result['funding']=funding['canonical_receipt'];assert balance(public['owner'])==5000;save()
 earned=next(x for x in o.rows('eosio.key','eosio.key','feebuckets')if x['owner']==BP);result['earnings_before']=earned;save()
 for label,operation,to in [('key-to-key','ksend',public['receiver']),('key-to-named','withdraw','gptgenesisbp')]:
  w=keywallet(public['owner']);request={'from':public['owner'],'to':to,'token_contract':'eosio.token','quantity':'0.0001 AIC','memo':'relay2 10percent qualification','auth_epoch':str(w['auth_epoch']),'request_id':hashlib.sha256(('relay2-'+label+'-20261111').encode()).hexdigest(),'relayer':BP,'reward_to':BP,'max_fee':'0.1000 AIC','relay_fee_bps':1000}
  body=node("import{KeyWalletClient,signPrepared}from './sdk/index.mjs';let s='';for await(const c of process.stdin)s+=c;const d=JSON.parse(s);const c=new KeyWalletClient('http://127.0.0.1:8888');await c.load();console.log(JSON.stringify(signPrepared(await c.prepare(d.operation,d.request),c.config,d.key)));",{'operation':operation,'request':request,'key':owner['Private key']});tid=body['prepared']['transaction_id'];o.save(P/(label+'-owner-envelope.json'),body);start=o.guard()['last_irreversible_block_num'];o.save(R/'qa-packets'/(label+'.json'),{'status':'SIGNED_IMMUTABLE','transaction_id':tid,'start_LIB':start,'expiration':body['prepared']['transaction']['expiration']});before=balance(public['owner'])
  q=urllib.request.Request(endpoint+'/submit',json.dumps(body).encode(),{'Content-Type':'application/json','User-Agent':'PureEOS-relay2-functional-QA/1.0'})
  with urllib.request.urlopen(q,timeout=30)as f:reply=json.load(f)
  o.save(P/(label+'-reply.json'),reply);assert reply['transaction_id']==tid;receipt=o.receipt(tid,start,timeout=180);fee=before-balance(public['owner'])-1;assert 0<fee<=1000
  deadline=time.monotonic()+90
  while True:
   history=fetch('https://aic-test.matrixai.one/history/v2/history/get_transaction?id='+tid);fees=[]
   def visit(v):
    if isinstance(v,dict):
     a=v.get('act')
     if isinstance(a,dict)and a.get('account')=='eosio.key'and a.get('name')=='feereceipt':fees.append(a['data'])
     for x in v.values():visit(x)
    elif isinstance(v,list):
     for x in v:visit(x)
   visit(history)
   if fees:break
   assert time.monotonic()<deadline;time.sleep(2)
  assert history['executed']and len(fees)==1;detail=fees[0];base=atoms(detail['ram_payment'])+atoms(detail['retained_base']);assert atoms(detail['total'])==fee==base+atoms(detail['relay_fee']);assert atoms(detail['relay_fee'])==max(1,(base*1000+9999)//10000)
  assert int(detail['allocated_bytes'])==(1536 if operation=='ksend'else 0)
  result['cases'].append({'label':label,'BP':BP,'endpoint':endpoint,'signed_rate_bps':1000,'principal':'0.0001 AIC','fee_atoms':fee,'actual_fee_receipt':detail,'canonical_receipt':receipt,'public_HTTPS_submission':True});save()
 result['earnings_after']=next(x for x in o.rows('eosio.key','eosio.key','feebuckets')if x['owner']==BP);expected=sum(atoms(x['actual_fee_receipt']['retained_base'])+atoms(x['actual_fee_receipt']['relay_fee'])for x in result['cases']);assert int(result['earnings_after']['relay_earned'])-int(result['earnings_before']['relay_earned'])>=expected;result['new_test_relay_income_atoms']=expected;o.guard();o.require_codes(o.NEW);o.authority_guard();result['status']='PASS';result['scope']=__doc__;save()
print('RELAY2_TWO_PUBLIC_HTTPS_PAYMENTS_10PERCENT_FEES_AND_CANONICAL_RECEIPTS_PASS')
