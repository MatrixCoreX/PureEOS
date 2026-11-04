"""Propose, collect owner vetoes, wait real chain time, exercise native rejection."""
from pathlib import Path
import datetime,json,subprocess,time,traceback,urllib.request
K=Path('/opt/pureeos-cap-veto-20261104-r1')
def save(name,value):(K/name).write_text(json.dumps(value,indent=2)+'\n')
def rpc(method):return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+method,data=b'{}',headers={'Content-Type':'application/json'}),timeout=40))
try:
 for operation in ['propose','veto']:subprocess.run(['python3','-B',str(K/'native_cap_governance.py'),operation],check=True)
 p=json.loads((K/'PROPOSAL.json').read_text());deadline=datetime.datetime.fromisoformat(p['governance']['execute_after'])
 while True:
  state=rpc('get_upgrade_state');info=rpc('get_info');assert info['chain_id']==p['chain_id']and state['pending']and state['proposal_id']==p['governance']['proposal_id']
  for key,value in p['proposal'].items():assert state[key]==value
  assert set(state['effective_vetoes'])==set(state['effective_proposal_voters'])and state['effective_veto_count']>=state['required_vetoes']>0
  remaining=max(0,(deadline-datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds())
  save('REVIEW_WAIT.json',{'status':'WAITING_NATIVE_MATURITY'if remaining else'MATURE','proposal_id':state['proposal_id'],'chain_time':info['head_block_time'],
   'remaining_seconds':remaining,'effective_vetoes':state['effective_veto_count'],'effective_voters':state['effective_voter_count'],'threshold':state['required_vetoes']})
  if not remaining:break
  time.sleep(min(30,max(1,remaining)))
 subprocess.run(['python3','-B',str(K/'native_cap_governance.py'),'attempt'],check=True)
 save('CAMPAIGN_RESULT.json',{'status':'PASS','result':'Full native review elapsed; exact cap payload rejected by unanimous effective BP veto','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()})
except BaseException:
 save('CAMPAIGN_FAILURE.json',{'status':'FAIL_STOPPED','error':traceback.format_exc(),'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()});raise
