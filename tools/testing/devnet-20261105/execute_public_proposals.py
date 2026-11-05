"""Wait real native review maturity; execute four exact capabilities separately once."""
from pathlib import Path
import datetime,hashlib,json,time,traceback,subprocess
from campaign_helpers import R,load_chain
f=load_chain();P=R/'review';J=R/'CREATION_EXECUTION_PROGRESS.json'
assert not J.exists(),'Existing execution needs per-ID canonical reconciliation, never replay'
assert json.loads((R/'CREATION_BROWSER_PUBLIC_pending.json').read_text())['status']=='PASS'
sub=json.loads((R/'CREATION_PROPOSALS_SUBMISSION.json').read_text());m=json.loads((P/'MANIFEST.json').read_text())
assert sub['chain_id']==m['chain_id']==f.rpc('get_info')['chain_id']
assert all(hashlib.sha256((P/n).read_bytes()).hexdigest()==h for n,h in m['files'].items())
rows={r['proposal_id']:r for r in sub['native_state']['pending_proposals']}
state={'status':'WAITING_NATIVE_REVIEW','chain_id':m['chain_id'],'confirmed':[],'initial_state':sub['native_state']}
def save():state['UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();J.write_text(json.dumps(state,indent=2)+'\n')
try:
 save();deadline=time.monotonic()+3600
 while True:
  now=f.rpc('get_system_account_state');assert now['latest_proposal_id']==4 and now['pending_count']==4
  assert all(r['effective_voter_count']and r['effective_veto_count']<r['required_vetoes']for r in now['pending_proposals'])
  info=f.rpc('get_info');lib=f.rpc('get_block',{'block_num_or_id':info['last_irreversible_block_num']})
  if all(r['review_mature']and datetime.datetime.fromisoformat(lib['timestamp'])>=datetime.datetime.fromisoformat(r['execute_after'])for r in now['pending_proposals']):break
  assert time.monotonic()<deadline,'Native maturity wait expired';time.sleep(10)
 runtime=json.loads((R/'PLAN.json').read_text());elf=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-3/libexec/nodeos');assert hashlib.sha256(elf.read_bytes()).hexdigest()==runtime['nodeos_SHA256']
 units=['pureeos-devnet-founder-20261105.service','pureeos-devnet-history-20261105.service']+['pureeos-devnet-bp-'+e['name']+'-20261105.service'for e in runtime['BPs'][1:]];processes=[]
 for u in units:
  pid=int(subprocess.check_output(['systemctl','show','-p','MainPID','--value',u],text=True));assert pid>0 and str(elf).encode()in Path('/proc',str(pid),'cmdline').read_bytes().split(b'\0')and str(elf)in Path('/proc',str(pid),'maps').read_text();processes.append({'unit':u,'PID':pid})
 state.update(status='EXECUTING_EXACT_REVIEWED_BYTES',mature_native_state=now,mature_irreversible_block={'block_num':lib['block_num'],'block_id':lib['id'],'timestamp':lib['timestamp']},runtime_pins={'nodeos_SHA256':runtime['nodeos_SHA256'],'processes':processes});save()
 password=(R/'founder/wallet/password.txt').read_text().strip().strip(chr(34))
 try:f.run(['wallet','unlock','-n','localtest'],input_text=password+'\n')
 except RuntimeError as e:
  if 'already unlocked'not in str(e).lower():raise
 for name in ['eosio.key','usd','cny','eosio.signup']:
  b=json.loads((P/(name+'.bundle.json')).read_text());native=f.rpc('get_system_account_state');r=next(r for r in native['pending_proposals']if r['proposal_id']==b['expected_proposal_id'])
  assert r['target']==name and r['review_mature']and r['review_seconds']==1800 and r['effective_voter_count']and r['effective_veto_count']<r['required_vetoes']
  for field in ['target','resource_payer','ram_payment','authority_version','code_bytes','abi_bytes','code_hash','abi_hash','review_document_hash','proposed_at','execute_after','proposal_voters']:assert r[field]==rows[r['proposal_id']][field]
  assert (datetime.datetime.fromisoformat(native['head_block_time'])-datetime.datetime.fromisoformat(r['proposed_at'])).total_seconds()>=1800
  assert f.rpc('get_code_hash',{'account_name':'eosio'})['code_hash']==r['system_code_hash']and f.rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']==r['token_code_hash']
  state['current_proposal_id']=r['proposal_id'];state['before_execution']=native;save()
  packet=R/'founder'/('reviewed-execution-'+name+'.json');assert not packet.exists();packet.write_text(json.dumps({'actions':b['execution_actions']}))
  result=f.tx(['push','transaction',str(packet),'--compression','zlib'])
  actual=f.rpc('get_code_hash',{'account_name':name});raw=f.rpc('get_raw_abi',{'account_name':name});account=f.rpc('get_account',{'account_name':name});assert actual['code_hash']==b['code_sha256']and raw['abi_hash']==b['raw_abi_sha256']and not account['privileged']
  perms={p['perm_name']:p['required_auth']for p in account['permissions']};owner=perms['owner'];active=perms['active']
  assert not owner['keys']and not owner['waits']and not active['keys']and not active['waits']
  if b['authority_version']==3:
   assert owner['threshold']==active['threshold']==1 and owner['accounts']==[{'permission':{'actor':'eosio.dev','permission':'owner'},'weight':1}]
   assert {(a['permission']['actor'],a['permission']['permission'],a['weight'])for a in active['accounts']}=={('eosio.dev','active',1),(name,'eosio.code',1)}
   assert name not in f.governance()['protected_accounts']
  else:
   assert owner['threshold']==1 and not owner['accounts']and active['threshold']==1 and active['accounts']==[{'permission':{'actor':name,'permission':'eosio.code'},'weight':1}]
   assert name in f.governance()['protected_accounts']
  entry={'target':name,'proposal_id':r['proposal_id'],'canonical_receipt':f.report['confirmed_transactions'][-1],'review_elapsed_chain_seconds':(datetime.datetime.fromisoformat(native['head_block_time'])-datetime.datetime.fromisoformat(r['proposed_at'])).total_seconds(),'code_hash':actual['code_hash'],'raw_ABI_hash':raw['abi_hash'],'ram_quota':account['ram_quota'],'ram_usage':account['ram_usage'],'permissions':account['permissions'],'privileged':False}
  state['confirmed'].append(entry);save();print('EXECUTION_IRREVERSIBLE '+name+' '+result['transaction_id'],flush=True)
 assert f.rpc('get_system_account_state')['pending_count']==0
 assert f.table('config',code='eosio.key',scope='eosio.key')[0]['version']==2
 for name,sym in [('usd','USD'),('cny','CNY')]:
  stats=f.rpc('get_currency_stats',{'code':name,'symbol':sym})[sym];assert stats['issuer']==name and stats['supply']=='10000.0000 '+sym and stats['max_supply']=='1000000.0000 '+sym
 state.update(status='FOUR_CREATIONS_DEPLOYED_IRREVERSIBLE',native_final_state=f.rpc('get_system_account_state'))
 save();(R/'CREATION_EXECUTION_RESULT.json').write_text(json.dumps(state,indent=2)+'\n')
except BaseException:
 state.update(status='NEEDS_CANONICAL_RECONCILIATION',error=f.redact(traceback.format_exc()));save();raise
