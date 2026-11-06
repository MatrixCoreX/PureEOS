"""Bounded full native review follow-through; exact one upgrade, no review bypass."""
from pathlib import Path
import json,subprocess,time,traceback,datetime
import ops_common as o
R=o.R;G=R/'governance/key'
try:
 assert not(R/'UPGRADE_WAITER_STARTED.json').exists();o.save(R/'UPGRADE_WAITER_STARTED.json',{'status':'WAITING_FULL_NATIVE_REVIEW_AND_CLIENTS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()})
 proposal=json.loads((G/'PROPOSAL.json').read_text());assert proposal['status']=='PROPOSED_IRREVERSIBLE';pid=proposal['governance']['proposal_id'];deadline=time.monotonic()+10800
 while True:
  assert time.monotonic()<deadline,'Bounded upgrade wait exceeded; no new proposal'
  info=o.guard();state=o.rpc('get_upgrade_state');assert state['pending']and state['proposal_id']==pid and state['target']=='eosio.key'
  assert all(state[k]==v for k,v in proposal['proposal'].items()),'Immutable review/code payload changed'
  assert state['review_seconds']==1800 and state['effective_voter_count']>0 and not state['execution_blocked_no_electorate']
  assert state['effective_veto_count']<state['required_vetoes'],'Effective BP veto blocks execution'
  lib=o.rpc('get_block',{'block_num_or_id':info['last_irreversible_block_num']});mature=o.stamp(lib['timestamp'])>=o.stamp(state['execute_after'])
  ready=(R/'CLIENT_PREACTIVATION.json').exists()and json.loads((R/'CLIENT_PREACTIVATION.json').read_text())['status']=='PASS'
  o.save(R/'UPGRADE_WAIT_PROGRESS.json',{'status':'WAITING','proposal_id':pid,'execute_after':state['execute_after'],'LIB_timestamp':lib['timestamp'],'mature':mature,'clients_ready':ready,'effective_voter_count':state['effective_voter_count'],'required_vetoes':state['required_vetoes'],'effective_veto_count':state['effective_veto_count']})
  if mature and ready:break
  time.sleep(20)
 assert not(G/'EXECUTE_PROGRESS.json').exists()and not(G/'EXECUTION.json').exists(),'Reconcile exact existing execution packet'
 with(R/'upgrade-execution.log').open('a')as log:subprocess.run(['python3','-B',str(R/'controllers/governance.py'),'key','execute'],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=900)
 execution=json.loads((G/'EXECUTION.json').read_text());assert execution['status']=='UPGRADED_IRREVERSIBLE'and execution['new_code_hash']==o.NEW
 with(R/'client-activation.log').open('a')as log:subprocess.run(['python3','-B',str(R/'controllers/activate_clients.py')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
 assert json.loads((R/'CLIENTS_ACTIVATED.json').read_text())['status']=='PASS'
 o.save(R/'GOVERNED_UPGRADE_AND_APPS.json',{'status':'PASS','execution':execution,'apps':json.loads((R/'CLIENTS_ACTIVATED.json').read_text()),'post_upgrade_public_payments':'PENDING; no previous payment replay'})
except BaseException:
 o.save(R/'UPGRADE_WAIT_FAILURE.json',{'status':'FAIL','error':traceback.format_exc(),'do_not_repeat_signed_packets_or_controller':True});raise
