"""Read-only final invariants; no payment, vote, bond or node mutations."""
from pathlib import Path
import datetime,hashlib,json,subprocess,urllib.request
K=Path('/opt/pureeos-relay-execution-income-20261105-r1');C=Path('/opt/pureeos-devnet-20261105-r1')
before=json.loads((K/'BEFORE.json').read_text());execution=json.loads((K/'EXECUTION.json').read_text())
assert execution['status']=='UPGRADED_IRREVERSIBLE'
def rpc(method,data={}):
    q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,json.dumps(data).encode(),{'Content-Type':'application/json'})
    with urllib.request.urlopen(q,timeout=20)as r:return json.load(r)
info=rpc('get_info');assert info['chain_id']==before['chain_id']
code=rpc('get_raw_abi',{'account_name':'eosio.key'})
assert code['code_hash']=='1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d'
assert code['abi_hash']==before['raw_ABI_hash']
current={u:int(subprocess.check_output(['systemctl','show',u,'--property=MainPID','--value'],text=True))for u in before['node_pids']}
assert current==before['node_pids'],'Original current node PIDs changed'
for account,h in [('eosio','b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'),('eosio.token','f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c')]:
    assert rpc('get_code_hash',{'account_name':account})['code_hash']==h
gov=rpc('get_upgrade_state');assert not gov['pending']and gov['proposal_id']==execution['proposal_id']and gov['review_seconds']==1800
block=rpc('get_block',{'block_num_or_id':execution['canonical_receipt']['block_num']})
assert block['id']==execution['canonical_receipt']['block_id']and block['block_num']<=info['last_irreversible_block_num']
tx=next(t for t in block['transactions']if(t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==execution['transaction_id']);assert tx['status']=='executed'
proposal=json.loads((K/'PROPOSAL.json').read_text())
elapsed=(datetime.datetime.fromisoformat(block['timestamp'])-datetime.datetime.fromisoformat(proposal['governance']['proposed_at'])).total_seconds();assert elapsed>=1800
table_pages=[]
def table(n):
    rows=[];lower=''
    for page in range(100):
        value=rpc('get_table_rows',{'code':'eosio.key','scope':'eosio.key','table':n,'json':True,'limit':100,'lower_bound':lower})
        table_pages.append({'table':n,'lower_bound':lower,'rows':len(value['rows']),'more':bool(value.get('more')),'next_key':value.get('next_key')})
        rows+=value['rows']
        if not value.get('more'):return rows
        next_key=str(value.get('next_key',''));assert next_key and next_key!=lower,'Incomplete table read'
        lower=next_key
    raise AssertionError('Table pagination incomplete')
old={b['owner']:b for b in before['current_fee_buckets']};now={b['owner']:b for b in table('feebuckets')}
assert int(now['gptgenesisbp']['network'])==int(old['gptgenesisbp']['network'])==1680
assert int(now['eosio.key']['network'])>=int(old['eosio.key']['network'])==672
assert not table('pending')and not table('nativefees')
relays=table('relays');assert len(relays)==5 and[(x['bp'],x['endpoint'])for x in relays if x['enabled']]==[('gptgenesisbp','https://aic-test.matrixai.one/relay')]
for u in ['pureeos-devnet-ordinary-activity-20261105.service','pureeos-devnet-purehistory-api-20261105.service','pureeos-devnet-purehistory-indexer-20261105.service','pureeos-devnet-key-history-20261105.service','pureeos-devnet-relay-20261105.service']:
    assert subprocess.run(['systemctl','is-active','--quiet',u]).returncode==0,u
qa=json.loads((K/'PUBLIC_TRANSFER_RESULT.json').read_text());assert qa['status']=='PASS'and qa['reserves_exact']
out={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'code_hash':code['code_hash'],'raw_ABI_hash':code['abi_hash'],'node_pids':current,'original25PIDs_unchanged':True,'upgrade_transaction_id':execution['transaction_id'],'upgrade_block':block['block_num'],'upgrade_block_id':block['id'],'review_elapsed_chain_seconds':elapsed,'native_CPU_us':tx['cpu_usage_us'],'native_NET_words':tx['net_usage_words'],'historical_founder_network_reserve_atoms':1680,'current_fee_buckets':list(now.values()),'sole_relay_and_ordinary_history_running':True,'scope':'Application-only TEST upgrade; no reset, TPS, BP/vote/bond/cap/native-system changes'}
out['complete_table_pages']=table_pages
(K/'FINAL_INVARIANTS.json').write_text(json.dumps(out,indent=2)+'\n');print('FINAL_READONLY_INVARIANTS_PASS')
