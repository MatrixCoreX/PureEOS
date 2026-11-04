"""Read-only before/after audit; never controls node/BP/activity services."""
from pathlib import Path
import datetime, hashlib, json, subprocess, sys, urllib.request

K = Path('/opt/pureeos-key-rate-20261104-r1')
R = Path('/opt/pureeos-keynet-20261103-r2')
phase = sys.argv[1]
assert phase in ['before', 'after']
def rpc(method, body={}):
    q = urllib.request.Request('http://127.0.0.1:8890/v1/chain/' + method,
        data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
    return json.load(urllib.request.urlopen(q, timeout=30))
def table(name):
    result = rpc('get_table_rows', {'code':'eosio','scope':'eosio','table':name,'json':True,'limit':1000})
    assert not result['more']
    return result['rows']
info = rpc('get_info')
assert info['chain_id'] == '116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
assert info['head_block_num'] - info['last_irreversible_block_num'] < 80
assert abs((datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) - datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds()) < 60
units = subprocess.check_output(['systemctl','list-units','--all','--type=service','--plain','--no-legend','pureeos-keynet2-*'], text=True)
units = [line.split() for line in units.splitlines() if line.strip()]
nodes = [x[0] for x in units if '-bp-' in x[0] or x[0] in ['pureeos-keynet2-founder-20261103.service','pureeos-keynet2-history-20261103.service']]
assert len(nodes) == 26
binary = R / 'pureeos-test-1.0.0-test.1/libexec/nodeos'
nodehash = hashlib.sha256(binary.read_bytes()).hexdigest()
assert nodehash == '0be5282d1d5a5eb72081e1d1374931d5e79cff139de728f8d230e29df50534e7'
pids, config = {}, {}
for unit in nodes:
    pid = int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True))
    assert pid > 0 and str(binary) in (Path('/proc')/str(pid)/'maps').read_text()
    pids[unit] = pid
    text = subprocess.check_output(['systemctl','cat',unit],text=True)
    config[unit] = hashlib.sha256(text.encode()).hexdigest()
assert len(set(pids.values())) == 26
business = [x[0] for x in units if any(s in x[0] for s in ['ordinary','activity','sender'])]
assert business
for name in [*business,'pureeos-key-history-20261103.service','pureeos-keynet2-relay-20261103.service','pureeos-keynet2-purehistory-api-20261103.service','pureeos-keynet2-purehistory-indexer-20261103.service']:
    assert subprocess.run(['systemctl','is-active','--quiet',name]).returncode == 0, name
selected = ['canadaboreal','cosmicgarden','deepseekcore','finlandhaven','gptgenesisbp']
producers = [{k:r[k] for k in ['owner','producer_key','is_active','total_votes']} for r in table('producersv2')]
assert len(producers) == 25 and all(r['is_active'] for r in producers)
stable = {'nodehash': nodehash, 'node_unit_hashes': config, 'producers': producers,
          'voters': table('votersv2'), 'bonds': table('bpbonds'),
          'permissions': {bp:rpc('get_account',{'account_name':bp})['permissions'] for bp in selected},
          'system_hash':rpc('get_code_hash',{'account_name':'eosio'})['code_hash'],
          'token_hash':rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']}
candidates = table('rotcands')
stable['permanent_bans'] = [r['owner'] for r in candidates if r['banned']]
assert stable['system_hash'] == 'b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
assert stable['token_hash'] == 'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
result = {'status':'PASS','phase':phase,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'chain_id':info['chain_id'],'head':info['head_block_num'],'LIB':info['last_irreversible_block_num'],
          'stable':stable,'node_count':len(nodes),'business_units':business,'node_pids':pids,
          'wallet_hash':rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash'],
          'registered_count':len(producers),'eligible_count':sum(not r['banned'] for r in candidates),
          'ban_discovery':'GERMANYCREST_AUTO_BAN_DISCOVERY.json',
          'resource_scope':'Functional checks only; no capacity/TPS claim'}
if phase == 'after':
    before = json.loads((K/'LIVE_AUDIT_before.json').read_text())
    assert before['stable'] == stable, 'Protected BP votes/bonds/keys/unit definitions changed'
    pause_file = K/'CHAIN_PAUSE_COMPLETED.json'
    if pause_file.exists():
        pause = json.loads(pause_file.read_text())
        assert pause['status'] == 'CHAIN_AND_HISTORY_PAUSED' and set(pause['nodes']) == set(nodes)
        result['user_authorized_coordinated_restart'] = True
        result['previous_node_pids'] = before['node_pids']
    else:
        assert before['node_pids'] == pids, 'A protected node restarted'
    assert result['wallet_hash'] == json.loads((K/'EXECUTION.json').read_text())['new_code_hash']
    recovery=json.loads((K/'STALE_RECOVERY.json').read_text())
    assert recovery['status']=='RECOVERED' and recovery['config_restored']
    assert hashlib.sha256((R/'bps/anthropiclab/config/config.ini').read_bytes()).hexdigest()==recovery['original_config_sha256']
    policy=rpc('get_finalizer_info')
    assert len(policy['active_finalizer_policy']['finalizers'])==21 and policy['active_finalizer_policy']['threshold']==15
    result['native_finalizer_policy']=policy
    def keyrows(name,scope='eosio.key'):
        q=rpc('get_table_rows',{'code':'eosio.key','scope':scope,'table':name,'json':True,'limit':1000})
        assert not q['more'];return q['rows']
    assets=keyrows('assets');wallets=keyrows('wallets');liabilities={int(a['id']):0 for a in assets}
    for wallet in wallets:
        for balance in keyrows('balances',str(wallet['id'])):
            liabilities[int(balance['asset_id'])]+=int(balance['amount'])
    credits=sum(int(x['amount']) for x in keyrows('nativefees'))
    buckets=sum(int(x['network'])+int(x['relay_earned']) for x in keyrows('feebuckets'))
    reserves=[]
    for a in assets:
        symbol=a['token_symbol'].split(',')[1]
        native=rpc('get_currency_balance',{'code':a['token_contract'],'account':'eosio.key','symbol':symbol})
        raw=int(native[0].split()[0].replace('.',''))
        expected=liabilities[int(a['id'])]+(credits+buckets if a['token_symbol']=='4,AIC' else 0)
        assert raw==int(a['reserve'])==expected and not a['busy']
        reserves.append({'token':a['token_contract'],'symbol':a['token_symbol'],'reserve_atoms':raw,'liability_atoms':expected})
    result['exact_vault_reserves']=reserves
    result['registered_relays']=keyrows('relays');result['relay_rates']=keyrows('relayrates')
    assert len(result['registered_relays'])==5
    assert [x['bp'] for x in result['registered_relays'] if x['enabled']]==['gptgenesisbp']
    result['memory']=[x for x in Path('/proc/meminfo').read_text().splitlines() if x.split(':')[0] in ['MemAvailable','SwapFree']]
    result['protected_state_unchanged'] = True
(K/('LIVE_AUDIT_'+phase+'.json')).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:result[k]for k in ['status','phase','head','LIB','node_count']}))
