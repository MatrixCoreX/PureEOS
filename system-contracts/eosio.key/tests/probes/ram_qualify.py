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
   if {v['value']for v in f['specification']}&{'GET_SENDER','RAM_RESTRICTIONS','DISABLE_DEFERRED_TRXS_STAGE_1','DISABLE_DEFERRED_TRXS_STAGE_2'}:activate(f['feature_digest'])
  for name in ['keyramattack','keyramnotify','keyfundvault','keyfunduserx']:
   self.tx(['create','account','eosio',name,self.accounts['eosio']['Public key']])
  for name in ['keyramattack','keyramnotify']:
   self.tx(['set','contract',name,str(args.artifacts),'keyramattack.wasm','keyramattack.abi'])
  self.action('init',{'vault':'keyfundvault'},'keyramattack',contract='keyramattack')
  self.bootstrapping=False
  self.report['scope']='TEST ONLY native RAM authority boundary demonstration; no wallet production qualification, no live changes, no TPS'
 def tests(self):
  def transfer(to):return self.action('transfer',{'from':'keyfundvault','to':to,'quantity':'1.0000 XTK','memo':'RAM authority probe'},'keyfundvault',contract='keyramattack')
  def usage():return int(self.rpc('get_account',{'account_name':'keyfundvault'})['ram_usage'])
  transfer('keyfunduserx');before=usage()
  self.action('setmode',{'value':1},'keyramattack',contract='keyramattack')
  transfer('keyfunduserx');after=usage()
  self.check(after-before>=4096,'Authorized malicious token first receiver can bill 4 KiB extra RAM to custody sender')
  self.report['demonstrated_RAM_delta_bytes']=after-before
  self.action('setmode',{'value':0},'keyramattack',contract='keyramattack')
  before=usage();state=self.table('accounts',code='keyramattack',scope='keyfundvault')
  self.action('transfer',{'from':'keyfundvault','to':'keyramnotify','quantity':'1.0000 XTK','memo':'notification RAM rejection'},'keyfundvault',contract='keyramattack',failure='notify context')
  self.check(usage()==before and self.table('accounts',code='keyramattack',scope='keyfundvault')==state,'RAM_RESTRICTIONS rejects recipient-notification RAM theft and rolls back principal')
  self.report['probe_WASM_SHA256']=hashlib.sha256((args.artifacts/'keyramattack.wasm').read_bytes()).hexdigest()
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
