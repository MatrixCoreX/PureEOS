"""Final read-only public release/preservation check; never submits a transaction."""
from pathlib import Path
import json,subprocess,urllib.request,datetime,hashlib,time
K=Path('/opt/pureeos-relay-execution-income-20261105-r1');C=Path('/opt/pureeos-devnet-20261105-r1')
for n in ['FINAL_UI_PUBLIC.json','PUBLIC_ASSET_DIGESTS.json']:assert json.loads((K/n).read_text())['status']=='PASS'
qa=json.loads((K/'FINAL_UI_PUBLIC.json').read_text());assert len(qa['variants'])==4 and all(v['canonical_cases']==12 and len(v['views'])==4 for v in qa['variants'])
publication=json.loads((K/'FRONTEND_PUBLICATION.json').read_text());assert publication['release']==str(Path('/var/www/aic-test/current').resolve())
assert json.loads((K/'PUBLIC_ASSET_DIGESTS.json').read_text())['files']==publication['files']
for n,h in publication['files'].items():assert hashlib.sha256((Path(publication['release'])/n).read_bytes()).hexdigest()==h
before=json.loads((K/'BEFORE.json').read_text());pids={u:int(subprocess.check_output(['systemctl','show',u,'-p','MainPID','--value'],text=True))for u in before['node_pids']};assert pids==before['node_pids']
units=['pureeos-devnet-ordinary-activity-20261105.service','pureeos-devnet-purehistory-api-20261105.service','pureeos-devnet-purehistory-indexer-20261105.service','pureeos-devnet-key-history-20261105.service','pureeos-devnet-relay-20261105.service']
def state(u):return dict(x.split('=',1)for x in subprocess.check_output(['systemctl','show',u,'-p','ActiveState,SubState,MainPID,NRestarts'],text=True).splitlines()if'='in x)
observations=[]
for sample in range(3):
 current={u:state(u)for u in units};assert all(v['ActiveState']=='active'and v['SubState']=='running'for v in current.values()),current
 observations.append(current)
 if sample<2:time.sleep(1)
for u in ['pureeos-devnet-wallet-20261105.service','pureeos-relay-income-preview-20261105.service']:assert state(u)['ActiveState']=='inactive'
def rpc(n,d={}):
 with urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+n,json.dumps(d).encode(),{'Content-Type':'application/json'}),timeout=20)as r:return json.load(r)
info=rpc('get_info');assert info['chain_id']==before['chain_id']
abi=rpc('get_raw_abi',{'account_name':'eosio.key'});assert abi['code_hash']=='1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d'and abi['abi_hash']==before['raw_ABI_hash']
gov=rpc('get_upgrade_state');assert not gov['pending']and gov['proposal_id']==1
fin=rpc('get_finalizer_info');assert len(fin['active_finalizer_policy']['finalizers'])==21 and fin['active_finalizer_policy']['threshold']==15
stats=rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC'];assert stats['supply']=='100000000.0000 AIC'and stats['max_supply']=='2100000000.0000 AIC'
assert info['last_irreversible_block_num']>json.loads((K/'CURRENT_AUDIT.json').read_text())['end']['last_irreversible_block_num']
activity=json.loads((C/'activity/STATE.json').read_text());activity={k:activity.get(k)for k in ['status','completed_batches','business_transactions','UTC','pending']}
# Do not export pending packets; only the original batch ordinal is relevant here.
activity['pending_batch']=(activity.pop('pending')or{}).get('batch')
assert activity['completed_batches']>=351,'Observe recovery of original batch without resending it'
rejections=C/'activity/REJECTED_BATCHES.jsonl';last_rejection=None
if rejections.exists()and rejections.read_text().splitlines():last_rejection=json.loads(rejections.read_text().splitlines()[-1])
result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'head':info['head_block_num'],'LIB':info['last_irreversible_block_num'],'node_pids':pids,'original25_PIDs_unchanged':True,'actual_native_policy':'21/15','AIC_stats':stats,'live_units_observations':observations,'ordinary_activity':activity,'ordinary_last_rejection':last_rejection,'temporary_preview_and_founder_signer_stopped':True,'release':publication['release'],'public_variants':4,'public_views':16,'canonical_payment_cases':12,'public_asset_digests':publication['files'],'retained_observation':'Initial one-shot all-services-active assertion failed during an ordinary sender transition. Follow-up sampled services active and completed batch >=351. Existing rejection/restart history retained; no controller or payment replay by this verifier.','scope':'Read-only current state after publication. No governance/payment replay, node reset/pause or new servers. Not a no-rejections/capacity qualification.'}
assert not(K/'PUBLIC_RELEASE_VERIFIED.json').exists();(K/'PUBLIC_RELEASE_VERIFIED.json').write_text(json.dumps(result,indent=2)+'\n');print('PUBLIC4_ASSETS6_CURRENT25PID_FINAL_RELEASE_PASS')
