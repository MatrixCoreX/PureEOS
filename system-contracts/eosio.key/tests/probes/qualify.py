#!/usr/bin/env python3
"""TEST ONLY native proof capabilities; new private chain, no TPS/system upgrade."""
import argparse,datetime,hashlib,json,os,shlex,subprocess,sys,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for a in ['package','work','artifacts','sdk-tests']:p.add_argument('--'+a,type=Path,required=True)
args=p.parse_args();sys.path.insert(0,str(args.sdk_tests))
from pureeos_single_choice_local_test import LocalTest
source=Path(__file__).resolve().parents[2]
args.work.mkdir(mode=0o700,parents=True,exist_ok=False)
for n in ['nodeos','cleos','keosd','helios-util']:
 d=args.work/'programs'/n;d.mkdir(parents=True);f=d/n
 f.write_text('#!/bin/sh\nexec '+shlex.quote(str(args.package/'bin'/n))+' "$@"\n');f.chmod(0o700)
class Probe(LocalTest):
 def tx(self,command,*,failure=None):
  for attempt in range(1,6):
   try:return super().tx(command,failure=failure)
   except RuntimeError as e:
    if failure is not None or not getattr(self,'bootstrapping',True)or'tx_cpu_usage_exceeded'not in str(e)or attempt==5:raise
    self.report.setdefault('bootstrap_CPU_retries',[]).append({'command':command[:3],'attempt':attempt,'error':'atomic bootstrap CPU deadline at unchanged 150ms cap'})
    print('RETRY atomic bootstrap CPU deadline',flush=True);self.wait_lib(self.rpc('get_info')['head_block_num']+2)
 def setup(self):
  self.accounts={'eosio':self.key()};self.owner=self.key();self.bls={'eosio':self.key(True)}
  (self.root/'config').mkdir();(self.root/'wallet').mkdir()
  profile=(args.package/'config/test-low-cost/config.ini').read_text().splitlines()
  profile=[x for x in profile if not x.startswith(('http-server-address','p2p-listen-endpoint'))]
  profile+=['plugin = eosio::producer_api_plugin','producer-name = eosio','enable-stale-production = true',
   'http-server-address = 127.0.0.1:'+str(self.http_port),'p2p-listen-endpoint = 127.0.0.1:'+str(self.p2p_port),'verbose-http-errors = true']
  for k in [self.accounts['eosio'],self.bls['eosio']]:profile.append('signature-provider = '+k['Public key']+'=KEY:'+k['Private key'])
  (self.root/'config/config.ini').write_text('\n'.join(profile)+'\n');(self.root/'config/config.ini').chmod(0o600)
  genesis={'initial_timestamp':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000'),
   'initial_key':self.accounts['eosio']['Public key'],'initial_configuration':json.loads((args.package/'config/test-low-cost/chain-parameters.json').read_text()),
   'initial_savanna':{'version':2,'finalizer_key':self.bls['eosio']['Public key'],'proof_of_possession':self.bls['eosio']['Proof of Possession']}}
  (self.root/'genesis.json').write_text(json.dumps(genesis))
  self.wallet=self.spawn([str(args.package/'bin/keosd'),'--data-dir',str(self.root/'wallet'),'--config-dir',str(self.root/'wallet'),'--http-server-address',''],'wallet.log')
  for _ in range(100):
   if(self.root/'wallet/keosd.sock').exists():break
   time.sleep(.1)
  self.run(['wallet','create','-n','localtest','--file',str(self.root/'wallet/password.txt')])
  self.run(['wallet','import','-n','localtest'],input_text=self.accounts['eosio']['Private key']+'\n')
  self.start_node('genesis',['--genesis-json',str(self.root/'genesis.json')]);info=self.wait_lib(17,timeout=90)
  self.report['chain_id']=info['chain_id'];self.report['scope']='Native transaction-ID/CFD owner-proof capability probe; no system replacement, no TPS, no live-chain mutation'
  self.tx(['set','contract','eosio',str(self.contracts/'eosio.bios')])
  supported=self.rpc('get_supported_protocol_features',{'exclude_disabled':True},namespace='producer');byid={x['feature_digest']:x for x in supported}
  active={x['feature_digest']for x in self.rpc('get_activated_protocol_features',{'limit':1000})['activated_protocol_features']}
  def activate(d):
   if d in active:return
   for dep in byid[d]['dependencies']:activate(dep)
   self.action('activate',{'feature_digest':d},'eosio');active.add(d)
  for f in supported:
   if {v['value']for v in f['specification']}&{'GET_SENDER','DISABLE_DEFERRED_TRXS_STAGE_1','DISABLE_DEFERRED_TRXS_STAGE_2'}:activate(f['feature_digest'])
  self.tx(['create','account','eosio','keyproofdemo',self.accounts['eosio']['Public key']])
  auth={'threshold':1,'keys':[{'key':self.accounts['eosio']['Public key'],'weight':1}],
   'accounts':[{'permission':{'actor':'keyproofdemo','permission':'eosio.code'},'weight':1}],'waits':[]}
  self.tx(['set','account','permission','keyproofdemo','active',json.dumps(auth),'owner'])
  self.tx(['set','contract','keyproofdemo',str(args.artifacts),'keyproofdemo.wasm','keyproofdemo.abi'])
  self.deployment=hashlib.sha256(os.urandom(32)).hexdigest()
  self.action('init',{'chain_id':info['chain_id'],'deployment':self.deployment},'keyproofdemo',contract='keyproofdemo')
  self.bootstrapping=False
  self.report['probe_script_SHA256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
  self.report['sign_script_SHA256']=hashlib.sha256((source/'tests/probes/sign.mjs').read_bytes()).hexdigest()
 def packet(self,**patch):
  info=self.rpc('get_info');data={'abi':str(args.artifacts/'keyproofdemo.abi'),'owner_key':self.owner['Private key'],
   'payer_key':self.accounts['eosio']['Private key'],'info':info,'chain_id':info['chain_id'],'deployment':self.deployment}|patch
  r=subprocess.run(['node',str(source/'tests/probes/sign.mjs')],input=json.dumps(data),capture_output=True,text=True,timeout=40)
  assert r.returncode==0,self.redact(r.stderr);return json.loads(r.stdout)
 def total(self):return self.table('total',code='keyproofdemo',scope='keyproofdemo')
 def send(self,packet,failure=None):
  lib=self.rpc('get_info')['last_irreversible_block_num'];error=None
  try:r=self.rpc('push_transaction',packet['packet'])
  except Exception as e:error=str(e)
  if failure:
   self.check(error is not None and failure.lower()in error.lower(),'Expected native proof rejection: '+failure);return
  if error:raise RuntimeError(error)
  assert r['processed']['receipt']['status']=='executed'
  receipt=self.wait_transaction(packet['id'],lib,timeout=90);self.report.setdefault('confirmed_transactions',[]).append(receipt)
 def tests(self):
  first=self.packet(value=1);self.send(first);self.check(self.total()[0]['count']==1,'CFD owner proof executes under existing TEST runtime without a native owner account')
  self.send(first,failure='duplicate');self.check(self.total()[0]['count']==1,'Native transaction-ID duplicate protection prevents a second execution')
  before=self.total()
  for field in ['expiration','value']:
   altered=self.packet(template=first['transaction'],proof_override=first['proof'],tamper=field)
   self.send(altered,failure='recovered key');self.check(self.total()==before,'Owner proof binds native '+field+'; BP re-signing cannot change it')
  poisoned=self.packet(value=2);bad=self.packet(template=poisoned['transaction'],proof_override=poisoned['proof'],tamper='proof')
  self.send(bad,failure='signature');self.send(poisoned)
  self.check(self.total()[0]['value']==3,'Invalid CFD does not poison a valid packet with the same native transaction ID')
  recursive=self.packet(value=1,recurse=True);before=self.total();self.send(recursive,failure='top-level wallet action')
  self.check(self.total()==before,'Inline re-entry cannot reuse a valid whole-transaction owner proof')
  expired=self.packet(ttl=-3);self.send(expired,failure='expired')
  a,b=self.packet(value=3),self.packet(value=4);lib=self.rpc('get_info')['last_irreversible_block_num']
  from concurrent.futures import ThreadPoolExecutor
  with ThreadPoolExecutor(max_workers=2)as pool:
   responses=list(pool.map(lambda x:self.rpc('push_transaction',x['packet']),[a,b]))
  for packet,result in zip([a,b],responses):
   assert result['processed']['receipt']['status']=='executed'
   self.report.setdefault('confirmed_transactions',[]).append(self.wait_transaction(packet['id'],lib,timeout=90))
  self.check(self.total()[0]['count']==4 and self.total()[0]['value']==10,'Independent random-ID transactions from the same public key both finalize without a shared sequence')
  self.report['probe_WASM_SHA256']=hashlib.sha256((args.artifacts/'keyproofdemo.wasm').read_bytes()).hexdigest()
test=Probe(args.work,args.package/'contracts')
try:test.setup();test.tests();test.report['status']='PASS'
except Exception as e:test.report['status']='FAIL';test.report['error']=test.redact(str(e));print('FAIL '+test.redact(str(e)),flush=True)
finally:
 for child in [test.node,test.wallet]:
  try:test.stop(child)
  except Exception as e:test.report.setdefault('shutdown_errors',[]).append(str(e));test.report['status']='FAIL'
 for f in test.logs:f.close()
 (args.work/'RESULT.json').write_text(json.dumps(test.report,indent=2)+'\n');print('RESULT '+test.report['status'],flush=True)
raise SystemExit(0 if test.report['status']=='PASS'else 1)
