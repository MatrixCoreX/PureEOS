"""Submit the already-published four exact proposals once; never execute here."""
from pathlib import Path
import datetime,hashlib,json,traceback
from campaign_helpers import R,load_chain,transfer
f=load_chain();P=R/'review';receipt=R/'CREATION_PROPOSALS_SUBMISSION.json';progress=R/'CREATION_PROPOSALS_SUBMISSION_PROGRESS.json'
assert not receipt.exists()and not progress.exists(),'Reconcile existing immutable packets/native proposals before resuming'
pub=json.loads((P/'GIT_PUBLICATION.json').read_text());assert pub['status']=='PUBLISHED_VERIFIED_REMOTE'
assert hashlib.sha256((P/'MANIFEST.json').read_bytes()).hexdigest()==pub['manifest_sha256']
m=json.loads((P/'MANIFEST.json').read_text());assert f.rpc('get_info')['chain_id']==m['chain_id']
assert all(hashlib.sha256((P/n).read_bytes()).hexdigest()==h for n,h in m['files'].items())
state=f.rpc('get_system_account_state');assert state['latest_proposal_id']==0 and state['pending_count']==0
names={e['name']for e in json.loads((R/'PLAN.json').read_text())['BPs']}
assert len(f.table('producersv2'))==24 and len(names)==24
bundles=[json.loads((P/(n+'.bundle.json')).read_text())for n in ['eosio.key','usd','cny','eosio.signup']]
# One exact delegation setup, followed by four refresh/propose pairs. The founder marker provides native execution resources.
actions=[transfer('gptgenesisbp','civicuserone','0.0001 AIC','TEST native proposal execution resources'),*bundles[0]['permission_setup_actions']]
for b in bundles:actions+=b['proposal_actions']
progress.write_text(json.dumps({'status':'SIGNING_ONCE','chain_id':m['chain_id'],'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'manifest_sha256':pub['manifest_sha256'],'publication':pub},indent=2)+'\n')
try:
 result=f.self_transact('four-published-independent-creation-proposals',actions)
 state=f.rpc('get_system_account_state');assert state['pending_count']==4 and state['latest_proposal_id']==4
 for row,b in zip(state['pending_proposals'],bundles):
  assert row['target']==b['target']and row['proposal_id']==b['expected_proposal_id']
  assert row['code_hash']==b['code_sha256']and row['abi_hash']==b['raw_abi_sha256']and row['review_document_hash']==b['native_review_commitment']
  assert row['authority_version']==b['authority_version']and row['review_seconds']==1800
  assert set(row['proposal_voters'])==names and row['effective_voter_count']==24 and row['required_vetoes']==16 and row['effective_veto_count']==0
 result={'status':'FOUR_PROPOSALS_IRREVERSIBLE_REVIEW_IN_PROGRESS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':m['chain_id'],'canonical_receipt':f.report['confirmed_transactions'][-1],'native_state':state,'publication':pub,'scope':'Four independent full1800chain-second review periods; no execution or shortcut'}
 receipt.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'status':result['status'],'transaction_id':result['canonical_receipt']['transaction_id'],'deadlines':[{k:r[k]for k in ['proposal_id','target','execute_after','required_vetoes']}for r in state['pending_proposals']]}),flush=True)
except BaseException:
 (R/'CREATION_PROPOSALS_SUBMISSION_FAILURE.json').write_text(json.dumps({'status':'NEEDS_CANONICAL_RECONCILIATION','error':f.redact(traceback.format_exc())},indent=2)+'\n');raise
