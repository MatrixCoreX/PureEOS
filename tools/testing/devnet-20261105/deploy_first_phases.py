"""One-shot authorized TEST cutover with immutable phase receipts. Stop on ambiguity."""
from pathlib import Path
import datetime,json,os,subprocess,time,traceback
R=Path('/opt/pureeos-devnet-20261105-r1');C=Path('/opt/pureeos-dev-account-genesis-20261105-r1')
os.umask(0o077);os.environ.update(PUREEOS_GENESIS_ROOT=str(R),PUREEOS_PRIVATE_GENESIS='0')
assert not(R/'DEPLOYMENT_STATE.json').exists(),'Existing phase journal requires canonical reconciliation'
state={'status':'PREPARED','completed':[],'old_data_policy':'Retain old raw blocks/state/history/volumes and reviewed bans/receipts in original isolated root; no old payment/controller replay','TPS':False}
def save():
 state['UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();(R/'DEPLOYMENT_STATE.json').write_text(json.dumps(state,indent=2)+'\n')
try:
 save()
 for label,args in [('archive-old',['freeze_old_chain.py']),('bootstrap',[str(C/'run_fresh_bootstrap.py')]),('history-node',['start_history_node.py']),('join24',['join_nodes.py','24']),('audit24',['audit_first_phases.py'])]:
  state.update(status='RUNNING',step=label);save();print('PHASE_START '+label,flush=True)
  p=Path(args[0]);p=p if p.is_absolute()else R/p
  q=subprocess.run(['python3','-B',str(p),*args[1:]],cwd=R)
  assert q.returncode==0,'Phase failed: '+label
  state['completed'].append(label);save();print('PHASE_PASS '+label,flush=True)
 state.update(status='FIRST_PHASES_PASS_AWAITING_PUBLIC_PROPOSALS');save()
except BaseException:
 state.update(status='NEEDS_CANONICAL_RECONCILIATION',error=traceback.format_exc());save();raise
