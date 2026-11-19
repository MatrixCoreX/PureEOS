from pathlib import Path
import json,sys,subprocess,datetime
R=Path('/opt/pureeos-testnet-bank-issuance-20261119-r1');sys.path.insert(0,str(R));import bank_upgrade as b
# Read only: do NOT call observe(), which writes the worker's heartbeat.
i=b.rpc('get_info');g=b.rpc('get_upgrade_state');f=b.rpc('get_finalizer_info',port=8902);lib=b.rpc('get_block',{'block_num_or_id':i['last_irreversible_block_num']});recent={v['description']for v in f['last_tracked_votes']if v['is_vote_strong']and(datetime.datetime.now(datetime.timezone.utc)-b.stamp(v['voted_for_block_timestamp'])).total_seconds()<12}
files={}
for n in ['PROPOSAL.json','MINT_AND_RESTORE_PROPOSAL.json','RESTORATION.json','WORKFLOW_STARTED.json','WORKFLOW_MINT_DONE.json','WORKFLOW_COMPLETE.json','WORKFLOW_HOLD.json','LATEST.json','WAIT_READ_WARNING.json','SIGNER_propose-r2_CLOSED.json','SIGNER_execute-mint-restore-proposal_CLOSED.json','SIGNER_restore_CLOSED.json']:
 p=R/n
 if p.is_file():files[n]=json.loads(p.read_text())
for p in (R/'transactions').glob('*.json'):files['transactions/'+p.name]=json.loads(p.read_text())
for p in R.glob('FAILURE_*.json'):files[p.name]=json.loads(p.read_text())
r={'utc':b.now(),'head':i['head_block_num'],'LIB':i['last_irreversible_block_num'],'head_age':(datetime.datetime.now(datetime.timezone.utc)-b.stamp(i['head_block_time'])).total_seconds(),'LIB_age':(datetime.datetime.now(datetime.timezone.utc)-b.stamp(i['last_irreversible_block_time'])).total_seconds(),'policy_count':len(f['active_finalizer_policy']['finalizers']),'threshold':f['active_finalizer_policy']['threshold'],'strong_recent':len(recent),'governance':g,'remaining':max(0,(b.stamp(g['execute_after'])-b.stamp(lib['timestamp'])).total_seconds())if g['pending']else 0,'token':b.token_pin(),'stats':b.stats(),'bank_units':b.bank(),'files':files,'worker':subprocess.check_output(['systemctl','show','pureeos-bank-issuance-20261119.service','-p','MainPID','-p','ActiveState','-p','SubState','-p','Result'],text=True),'auto_latest':json.loads(Path('/opt/pureeos-testnet-cpu-stability-20261119-r1/AUTO_REPLACEMENT_LATEST.json').read_text())};print(json.dumps(r))
