"""Fresh TEST bootstrap; private canary or explicitly authorized managed net."""
from pathlib import Path
import os,json,datetime,hashlib,subprocess,time,traceback
CONTROL=Path('/opt/pureeos-dev-account-genesis-20261105-r1')
exec(compile((CONTROL/'fresh_definitions.py').read_text(),'fresh-reviewed-definitions','exec'),globals())
f=None
try:
 assert json.loads((CONTROL/'qualification-6/RESULT.json').read_text())['status']=='PASS'
 approved=json.loads((CONTROL/'CORE_BUILD3_TEST_FIX4.json').read_text());assert approved['status']=='CORE_BUILT_TESTS_REBUILT_REQUIRES_QUALIFICATION'
 assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==approved['nodeos']['sha256']
 assert not(R/'BOOTSTRAP_RESULT.json').exists(),'Completed genesis is never repeated'
 f=TestChain();(R/'FOUNDER_PORTS.json').write_text(json.dumps({'rpc':f.http_port,'p2p':f.p2p_port})+'\n')
 f.setup(register_candidates=False);f.wait_produced('gptgenesisbp')
 f.action('voteproducer',{'voter':'gptgenesisbp','proxy':'','producers':['gptgenesisbp']},'gptgenesisbp')
 deadline=time.monotonic()+180
 while True:
  fin=f.rpc('get_finalizer_info')
  if len(fin['active_finalizer_policy']['finalizers'])==1 and fin['active_finalizer_policy']['finalizers'][0]['description']=='gptgenesisbp'and not fin.get('pending_finalizer_policy'):break
  assert time.monotonic()<deadline;time.sleep(.3)
 cfg=f.root/'config/config.ini';bootstrap=[f.accounts['eosio']['Public key'],f.bls['eosio']['Public key'],f.accounts['gptgenesisbp']['Public key']]
 lines=[l for l in cfg.read_text().splitlines()if l.strip()not in ['producer-name = eosio']and not any(l.startswith('signature-provider = '+k+'=')for k in bootstrap)]
 cfg.write_text('\n'.join(lines)+'\n')
 if PRIVATE:
  f.stop(f.node);f.start_node('after-genesis-handover')
 else:subprocess.run(['systemctl','restart','pureeos-devnet-founder-20261105.service'],check=True)
 deadline=time.monotonic()+90
 while True:
  try:info=f.rpc('get_info');break
  except Exception:assert time.monotonic()<deadline;time.sleep(.2)
 f.wait_lib(info['head_block_num']+6,timeout=90)
 state=f.governance();assert state['developer']=='eosio.dev'and state['sealed']and state['review_seconds']==1800
 absent=[]
 for name in ['eosio.key','eosio.signup','usd','cny']:
  try:f.rpc('get_account',{'account_name':name})
  except RuntimeError as e:
   assert 'unknown key'in str(e).lower()or'does not exist'in str(e).lower()or'unknown account'in str(e).lower(),str(e)
   absent.append(name)
  else:raise AssertionError('Post-startup service was precreated: '+name)
 auth=f.rpc('get_account',{'account_name':'eosio.dev'})['permissions']
 assert all(not p['required_auth']['keys']and not p['required_auth']['waits']and p['required_auth']['accounts']for p in auth if p['perm_name']in ['owner','active','upgrade'])
 assert f.table('global2')[0]['new_ram_per_block']==1024
 assert f.rpc('get_code_hash',{'account_name':'eosio'})['code_hash']=='b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
 assert f.table('bpbondcfg')[0]['enforced']
 result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'private_canary':PRIVATE,'first_BP':'gptgenesisbp','developer':'eosio.dev','developer_permissions':auth,'service_absence':absent,'governance':state,'native_creation_state':f.rpc('get_system_account_state'),'checks':f.checks,'confirmed_transactions':f.report.get('confirmed_transactions',[]),'bootstrap_stale_flag':'Retained only for initial sole-founder restart; public cutover must restore false after other actual producers/finalizers are live','scope':'Native fresh TEST genesis and founder handover; no TPS or capacity claim'}
 (R/'BOOTSTRAP_RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'status':'PASS','chain_id':info['chain_id'],'private_canary':PRIVATE}),flush=True)
except BaseException:
 error=traceback.format_exc();error=f.redact(error)if f else error
 (R/'BOOTSTRAP_FAILURE.json').write_text(json.dumps({'status':'FAIL','error':error},indent=2)+'\n');print(error,flush=True);raise
finally:
 if PRIVATE and f:
  for c in [f.node,f.wallet]:
   try:f.stop(c)
   except BaseException as e:print(f.redact(str(e)),flush=True)
