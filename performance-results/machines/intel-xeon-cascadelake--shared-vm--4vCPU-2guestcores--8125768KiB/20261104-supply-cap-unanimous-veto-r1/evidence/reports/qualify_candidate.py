"""Remote private namespace: prove the proposed code really doubles only AIC cap.
No public deployment, governance shortcut, TPS or live node controls.
"""
from pathlib import Path
import datetime,hashlib,json,os,shlex,subprocess,sys,time,traceback
os.umask(0o077)
K=Path('/opt/pureeos-cap-veto-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');P=R/'pureeos-test-1.0.0-test.1';A=K/'artifacts';B=Path('/opt/kcap-q/r2')
assert json.loads((K/'BUILD_RESULT.json').read_text())['status']=='PASS';assert not B.exists();B.parent.mkdir(mode=0o700,exist_ok=True);B.mkdir(mode=0o700)
for name in ['nodeos','cleos','keosd','helios-util']:
 d=B/'programs'/name;d.mkdir(parents=True);p=d/name;p.write_text('#!/bin/sh\nexec '+shlex.quote(str(P/'bin'/name))+' "$@"\n');p.chmod(0o700)
sys.path.insert(0,str(R/'harness'))
from pureeos_single_choice_local_test import LocalTest
class Test(LocalTest):
 local_signers=('eosio',)
 def __init__(self):
  super().__init__(B,P/'contracts');self.cleos=R/'native-client/cleos'
  self.report['qualification_script_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
  self.report['scope']='Private one-node cap migration functionality only; not public governance/parallel throughput qualification'
 def tx(self,args,*,failure=None):
  self.nonce+=1;folder=self.root/'immutable-packets';folder.mkdir(exist_ok=True);path=folder/(str(self.nonce)+'.json')
  signed=json.loads(self.run([*args,'-j','-x','1800','--dont-broadcast']));path.write_text(json.dumps(signed));packed=json.loads(self.run(['convert','pack_transaction',str(path)]))
  transaction_id=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest();info=self.rpc('get_info')
  command=['push','transaction',str(path),'-s','-j','--compression','zlib']
  raw=self.run(command,failure=failure)
  if failure is not None:
   after=self.wait_lib(info['head_block_num']+2,timeout=120)
   for height in range(info['last_irreversible_block_num']+1,after['last_irreversible_block_num']+1):
    assert not any((r['trx']if isinstance(r['trx'],str)else r['trx']['id'])==transaction_id for r in self.rpc('get_block',{'block_num_or_id':height})['transactions'])
   self.report.setdefault('rejected_transactions',[]).append({'transaction_id':transaction_id,'expected_error':failure,'canonical_absence_through_LIB':after['last_irreversible_block_num']});return
  result=json.loads(raw);assert result['transaction_id']==transaction_id and result['processed']['receipt']['status']=='executed'
  receipt=self.wait_transaction(transaction_id,info['last_irreversible_block_num'],timeout=120);self.report.setdefault('confirmed_transactions',[]).append(receipt);return result
 def stats(self,contract='eosio.token',symbol='AIC'):return self.rpc('get_currency_stats',{'code':contract,'symbol':symbol})[symbol]
 def setup(self):
  names=['eosio','capusersolar','capuserlunar','capusdtokenx'];self.accounts={n:self.key()for n in names};self.bls={'eosio':self.key(True)}
  (self.root/'config').mkdir();(self.root/'wallet').mkdir()
  profile=(P/'config/test-low-cost/config.ini').read_text().splitlines();profile=[l for l in profile if not l.startswith(('http-server-address','p2p-listen-endpoint','producer-name','signature-provider','abi-serializer-max-time-ms','http-max-response-time-ms'))]
  profile+=['plugin = eosio::producer_api_plugin','enable-stale-production = true','http-server-address = 127.0.0.1:'+str(self.http_port),'p2p-listen-endpoint = 127.0.0.1:'+str(self.p2p_port),'verbose-http-errors = true','abi-serializer-max-time-ms = 500','http-max-response-time-ms = 500','producer-name = eosio']
  for key in [self.accounts['eosio'],self.bls['eosio']]:profile.append('signature-provider = '+key['Public key']+'=KEY:'+key['Private key'])
  (self.root/'config/config.ini').write_text('\n'.join(profile)+'\n')
  genesis={'initial_timestamp':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000'),'initial_key':self.accounts['eosio']['Public key'],
   'initial_savanna':{'version':2,'finalizer_key':self.bls['eosio']['Public key'],'proof_of_possession':self.bls['eosio']['Proof of Possession']},
   'initial_configuration':json.loads((P/'config/test-low-cost/chain-parameters.json').read_text())}
  (self.root/'genesis.json').write_text(json.dumps(genesis));self.wallet=self.spawn([str(P/'bin/keosd'),'--data-dir',str(self.root/'wallet'),'--config-dir',str(self.root/'wallet'),'--http-server-address',''],'wallet.log')
  for attempt in range(100):
   if(self.root/'wallet/keosd.sock').exists():break
   assert self.wallet.poll()is None;time.sleep(.1)
  self.run(['wallet','create','-n','localtest','--file',str(self.root/'wallet/password.txt')]);self.secrets.append((self.root/'wallet/password.txt').read_text().strip())
  for key in self.accounts.values():self.run(['wallet','import','-n','localtest'],input_text=key['Private key']+'\n')
  self.start_node('genesis',['--genesis-json',str(self.root/'genesis.json')]);info=self.wait_lib(17,timeout=180);assert info['chain_id']!='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0';self.report['chain_id']=info['chain_id']
  for name in ['eosio.token',*names[1:]]:
   key=self.accounts.get(name,self.accounts['eosio'])['Public key'];self.tx(['create','account','eosio',name,key,key])
  self.tx(['set','contract','eosio.token',str(P/'contracts/eosio.token')])
  assert self.rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']=='f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
  self.action('create',{'issuer':'eosio','maximum_supply':'2100000000.0000 AIC'},'eosio.token',contract='eosio.token')
  self.action('issue',{'to':'eosio','quantity':'100000000.0000 AIC','memo':'private qualification only'},'eosio',contract='eosio.token')
  self.action('create',{'issuer':'eosio','maximum_supply':'1000000.0000 USD'},'eosio.token',contract='eosio.token')
  self.action('issue',{'to':'eosio','quantity':'100.0000 USD','memo':'private qualification only'},'eosio',contract='eosio.token')
 def tests(self):
  old=self.stats();assert old['max_supply']=='2100000000.0000 AIC';self.tx(['set','contract','eosio.token',str(A),'eosio.token.wasm','eosio.token.abi'])
  self.check(self.stats()==old,'Code installation alone leaves the old cap and supply unchanged')
  self.action('transfer',{'from':'eosio','to':'capusersolar','quantity':'1.0000 USD','memo':'non-AIC'},'eosio',contract='eosio.token')
  self.check(self.stats()==old and self.stats(symbol='USD')['max_supply']=='1000000.0000 USD','Non-AIC transfer never migrates AIC or USD caps')
  self.action('transfer',{'from':'eosio','to':'capusersolar','quantity':'100000001.0000 AIC','memo':'must roll back'},'eosio',contract='eosio.token',failure='overdrawn balance')
  self.check(self.stats()==old,'Overdrawn transfer rolls the preceding cap modification back atomically')
  self.action('transfer',{'from':'eosio','to':'capusersolar','quantity':'0.00000001 AIC','memo':'wrong precision'},'eosio',contract='eosio.token',failure='symbol precision mismatch')
  self.check(self.stats()==old,'Wrong precision cannot trigger a cap migration')
  self.action('transfer',{'from':'eosio','to':'capusersolar','quantity':'1.0000 AIC','memo':'first successful migration'},'eosio',contract='eosio.token')
  changed=self.stats();self.check(changed['max_supply']=='4200000000.0000 AIC'and changed['supply']==old['supply']and changed['issuer']==old['issuer'],'First successful AIC transfer doubles only max_supply; no mint or issuer change')
  self.check(self.rpc('get_currency_balance',{'code':'eosio.token','account':'capusersolar','symbol':'AIC'})==['1.0000 AIC'],'Recipient receives full principal')
  self.action('transfer',{'from':'capusersolar','to':'capuserlunar','quantity':'0.1000 AIC','memo':'repeat'},'capusersolar',contract='eosio.token')
  self.check(self.stats()==changed,'Repeated transfers do not double the cap again')
  self.tx(['set','contract','capusdtokenx',str(A),'eosio.token.wasm','eosio.token.abi'])
  self.action('create',{'issuer':'eosio','maximum_supply':'2100000000.0000 AIC'},'capusdtokenx',contract='capusdtokenx')
  self.action('issue',{'to':'eosio','quantity':'10.0000 AIC','memo':'other contract'},'eosio',contract='capusdtokenx')
  self.action('transfer',{'from':'eosio','to':'capuserlunar','quantity':'1.0000 AIC','memo':'other contract'},'eosio',contract='capusdtokenx')
  self.check(self.stats('capusdtokenx')['max_supply']=='2100000000.0000 AIC','Same symbol on another contract is outside the exact migration identity')
  self.report['final_stats']=self.stats();self.report['new_WASM_sha256']=hashlib.sha256((A/'eosio.token.wasm').read_bytes()).hexdigest()
t=Test()
try:t.setup();t.tests();t.report['status']='PASS'
except BaseException as error:t.report['status']='FAIL';t.report['error']=t.redact(str(error));print('FAIL '+t.redact(str(error)),flush=True)
finally:
 for child in [t.node,t.wallet]:
  try:t.stop(child)
  except Exception as error:t.report.setdefault('shutdown_errors',[]).append(str(error));t.report['status']='FAIL'
 for log in t.logs:log.close()
 (K/'QUALIFICATION_RESULT.json').write_text(json.dumps(t.report,indent=2)+'\n');print('PRIVATE CAP QUALIFICATION '+t.report['status'],flush=True)
sys.exit(0 if t.report['status']=='PASS'else 1)
