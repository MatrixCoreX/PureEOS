"""One read-only native review observation; never executes or advances time."""
from pathlib import Path
import datetime,json,urllib.request
K=Path('/opt/pureeos-dev-proposals-20261104-r1');proposal=json.loads((K/'PROPOSAL.json').read_text())
def rpc(method,data={}):
 return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'}),timeout=40))
s=rpc('get_upgrade_state');info=rpc('get_info');code=rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']
assert s['proposal_id']==proposal['governance']['proposal_id']and s['review_seconds']==1800
for key,value in proposal['proposal'].items():assert s[key]==value
observed=datetime.datetime.fromisoformat(info['head_block_time']);deadline=datetime.datetime.fromisoformat(s['execute_after'])
remaining=max(0,(deadline-observed).total_seconds())
assert info['chain_id']==proposal['chain_id']and s['pending']and code==s['old_code_hash']
record={'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_time':info['head_block_time'],'remaining_seconds':remaining,'pending':s['pending'],
 'proposal_id':s['proposal_id'],'effective_voters':s['effective_voter_count'],'effective_vetoes':s['effective_veto_count'],'required_vetoes':s['required_vetoes'],
 'execution_blocked_no_electorate':s['execution_blocked_no_electorate'],'deployed_code':code,'head':info['head_block_num'],'LIB':info['last_irreversible_block_num']}
with(K/'REVIEW_OBSERVATIONS.ndjson').open('a')as f:f.write(json.dumps(record)+'\n')
print(json.dumps(record))
