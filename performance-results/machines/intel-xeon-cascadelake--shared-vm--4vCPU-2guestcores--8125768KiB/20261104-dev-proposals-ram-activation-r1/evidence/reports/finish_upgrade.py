"""Observe native maturity, then execute the reviewed code and switch apps.
No early execution, chain controls, fee-test payments or ambiguous re-preparation.
"""
from pathlib import Path
import datetime,json,subprocess,time,traceback,urllib.request
K=Path('/opt/pureeos-dev-proposals-20261104-r1')
def save(name,value):(K/name).write_text(json.dumps(value,indent=2)+'\n')
def rpc(method,data={}):
 return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'}),timeout=40))
try:
 proposal=json.loads((K/'PROPOSAL.json').read_text());assert proposal['status']=='PROPOSED_IRREVERSIBLE'
 nav=json.loads((K/'navigation-public/RESULT.json').read_text());build=json.loads((K/'FRONTEND_RESULT.json').read_text());deployed=json.loads((K/'DEPLOYMENT.json').read_text())
 assert nav['status']==build['status']=='PASS'and nav['build_files']==build['files']==deployed['files']
 assert Path('/var/www/aic-test/current').resolve()==Path(deployed['release'])and deployed['status']=='PUBLISHED'
 assert not(K/'LEDGER_before.json').exists()
 while True:
  state=rpc('get_upgrade_state');info=rpc('get_info');assert info['chain_id']==proposal['chain_id']
  assert state['pending']and state['proposal_id']==proposal['governance']['proposal_id']and state['review_seconds']==1800
  for key,value in proposal['proposal'].items():assert state[key]==value
  assert state['effective_voter_count']>0 and not state['execution_blocked_no_electorate']and state['effective_veto_count']<state['required_vetoes']
  remaining=(datetime.datetime.fromisoformat(state['execute_after'])-datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds()
  save('REVIEW_WAIT.json',{'status':'WAITING_NATIVE_TIME'if remaining>0 else'NATIVE_TIME_MATURE','chain_time':info['head_block_time'],'remaining_seconds':max(0,remaining),
    'proposal_id':state['proposal_id'],'vetoes':state['effective_veto_count'],'required_vetoes':state['required_vetoes'],'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()})
  if remaining<=0:break
  time.sleep(min(30,max(1,remaining)))
 # Native devexec independently enforces time, electorate/veto and all hashes.
 for script,args in [('ledger_preservation.py',['before']),('native_governance.py',['execute']),('ledger_preservation.py',['after-upgrade']),('switch_apps.py',[])]:
  subprocess.run(['python3','-B',str(K/script),*args],check=True)
 save('NATIVE_FINISH.json',{'status':'UPGRADED_AND_APPS_SWITCHED','proposal_id':proposal['governance']['proposal_id'],'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
  'test_payments_sent':False,'chain_paused_or_restarted':False,'native_review_bypassed':False})
 print('Reviewed native upgrade and matching application switch complete',flush=True)
except BaseException:
 save('NATIVE_FINISH_FAILURE.json',{'status':'FAIL_STOPPED','error':traceback.format_exc(),'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()});raise
