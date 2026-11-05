"""Gated one-shot post-creation phases; stop on ambiguity instead of replaying payments."""
from pathlib import Path
import datetime,json,os,subprocess,time,traceback
R=Path('/opt/pureeos-devnet-20261105-r1');os.environ.update(PUREEOS_GENESIS_ROOT=str(R),PUREEOS_PRIVATE_GENESIS='0')
assert not(R/'POST_PHASES_STATE.json').exists(),'Reconcile per-phase canonical receipts before resuming'
state={'status':'WAITING_FOR_NATIVE_CREATIONS','completed':[]}
def save():state['UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();(R/'POST_PHASES_STATE.json').write_text(json.dumps(state,indent=2)+'\n')
try:
 save();deadline=time.monotonic()+3600
 while not(R/'CREATION_EXECUTION_RESULT.json').exists():
  p=R/'CREATION_EXECUTION_PROGRESS.json'
  if p.exists():assert json.loads(p.read_text())['status']!='NEEDS_CANONICAL_RECONCILIATION','Execution stopped; no post phase may submit'
  assert time.monotonic()<deadline;time.sleep(5)
 assert json.loads((R/'CREATION_EXECUTION_RESULT.json').read_text())['status']=='FOUR_CREATIONS_DEPLOYED_IRREVERSIBLE'
 for label,script in [('key-projection','setup_key_projection.py'),('sole-founder-relay','setup_key_service.py'),('three-route-transfer-QA','verify_public_transfers.py'),('signup-formats-QA','verify_public_signup.py'),('ordinary90','prepare_activity.py')]:
  state.update(status='RUNNING',step=label);save();print('POST_START '+label,flush=True)
  q=subprocess.run(['python3','-B',str(R/script)],cwd=R);assert q.returncode==0,'Post phase failed: '+label
  state['completed'].append(label);save();print('POST_PASS '+label,flush=True)
 state.update(status='POST_MUTATION_PHASES_PASS_READ_ONLY_FINAL_VERIFICATION_PENDING');save()
except BaseException:
 state.update(status='NEEDS_CANONICAL_RECONCILIATION',error=traceback.format_exc());save();raise
