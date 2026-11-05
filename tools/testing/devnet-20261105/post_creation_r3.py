"""Only new post-creation phases; existing90ordinary preparation is never replayed."""
from pathlib import Path
import datetime,json,subprocess,time,traceback,os
R=Path('/opt/pureeos-devnet-20261105-r1');J=R/'POST_CREATION_R3_PROGRESS.json'
assert not J.exists(),'Reconcile each completed phase and exact canonical IDs before any resume'
previous=json.loads((R/'POST_CREATION_R2_PROGRESS.json').read_text());assert previous['status']=='STOPPED_NEEDS_PER_STEP_RECONCILIATION'and previous['steps']==[]
state={'status':'WAITING_FOUR_EXACT_EXECUTIONS','steps':[],'scope':'Only new projection/sole founder relay and owned transfer/signup QA; preserve all current nodes/history/ordinary activity'}
def save():state['UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();J.write_text(json.dumps(state,indent=2)+'\n')
try:
 save();deadline=time.monotonic()+4000
 while not(R/'CREATION_EXECUTION_RESULT_r2.json').exists():
  p=R/'CREATION_EXECUTION_PROGRESS_r3.json'
  if p.exists():assert json.loads(p.read_text())['status']!='NEEDS_CANONICAL_RECONCILIATION','Execution stopped; no post phase authorized to proceed'
  assert time.monotonic()<deadline,'Bounded post-creation wait expired';time.sleep(10)
 assert json.loads((R/'CREATION_EXECUTION_RESULT_r2.json').read_text())['status']=='FOUR_CREATIONS_DEPLOYED_IRREVERSIBLE'
 for name,receipt,expected in [('setup_key_projection_r2.py','KEY_PROJECTION_SETUP.json','STARTED'),('setup_key_service_r2.py','KEY_SERVICE_RESULT.json','PASS'),('verify_public_transfers_r2.py','PUBLIC_TRANSFER_RESULT.json','PASS'),('verify_public_signup_r2.py','PUBLIC_SIGNUP_RESULT.json','PASS')]:
  assert not(R/receipt).exists(),'Preexisting phase receipt; reconcile instead of replay'
  state.update(status='RUNNING',current_step=name);save()
  with(R/(name+'.log')).open('w')as log:subprocess.run(['python3','-B',str(R/name)],cwd=R,env={**os.environ,'PUREEOS_GENESIS_ROOT':str(R)},stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
  proof=json.loads((R/receipt).read_text());assert proof['status']==expected
  state['steps'].append({'script':name,'receipt':receipt,'status':proof['status']});save();print('POST_PHASE_VERIFIED '+name,flush=True)
 state.update(status='PASS',current_step=None);save();(R/'POST_CREATION_R2_RESULT.json').write_text(json.dumps(state,indent=2)+'\n')
except BaseException:
 state.update(status='STOPPED_NEEDS_PER_STEP_RECONCILIATION',error=traceback.format_exc());save();raise
