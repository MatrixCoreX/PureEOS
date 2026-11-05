"""Read-only final audit for the six authorized feature-demo BPs; never enroll or pay."""
from common import *
import traceback

try:
    assert not (R/'FINAL_AUDIT.json').exists(), 'Retain the existing audit; do not overwrite it'
    result=read('NATIVE_DEMO_RESULT.json');assert result['status']=='PASS'
    plan=read('PLAN.json');before=read('BASELINE.json')
    previous=json.loads(Path('/opt/pureeos-relay-execution-income-20261105-r1/CURRENT_AUDIT.json').read_text())
    start=rpc('get_info');assert start['chain_id']==CHAIN
    pids=original_pids();accounts={}
    for name,expected in previous['accounts'].items():
        a=rpc('get_account',{'account_name':name})
        actual={'privileged':a['privileged'],'permissions':a['permissions']}
        assert actual==expected, 'Original authority changed: '+name
        accounts[name]=actual
    governance=rpc('get_upgrade_state')
    assert governance['sealed'] and governance['developer']=='eosio.dev' and not governance['pending'] and governance['review_seconds']==1800
    assert rpc('get_consensus_parameters')==previous['consensus_parameters']
    assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==BINARY
    original_nodes=[]
    for old in previous['nodes']:
        u=state(old['unit']);assert u['ActiveState']=='active' and int(u['MainPID'])==pids[old['unit']]
        proc=Path('/proc',u['MainPID']);assert str(V/'libexec/nodeos').encode() in (proc/'cmdline').read_bytes().split(b'\0')
        port=8890 if old['name']=='history' else next(x['rpc_port'] for x in json.loads((C/'PLAN.json').read_text())['BPs'] if x['name']==old['name'])
        i=rpc('get_info',port=port);assert i['chain_id']==CHAIN and i['last_irreversible_block_num']>=start['last_irreversible_block_num']-80
        original_nodes.append({'name':old['name'],'unit':old['unit'],'process':u,'info':i})
    old_archive={u:state(u) for u in previous['archived_old26_disabled']}
    assert len(old_archive)==26 and all(x['ActiveState']=='inactive' and x['MainPID']=='0' and x['UnitFileState']=='disabled' for x in old_archive.values())
    producers=table('producersv2');voters=table('votersv2');candidates=table('rotcands');bonds=table('bpbonds');maintenance=table('bpmaint')
    assert len(producers)==len(bonds)==30 and len({int(p['total_votes']) for p in producers})==30
    by_voter={x['owner']:x for x in voters};by_producer={x['owner']:x for x in producers};by_bond={x['owner']:x for x in bonds}
    assert all(by_voter[x['owner']]==x for x in before['old_voters'])
    assert all(by_producer[x['owner']]['total_votes']==x['total_votes'] for x in before['old_producers'])
    assert all(by_bond[x['owner']]==x for x in before['old_bonds'])
    votes=[]
    for node in plan['nodes']:
        name=node['name'];other=node['voter'];u=state(node['unit'])
        assert by_voter[name]['producers']==[name] and by_voter[other]['producers']==[name]
        assert int(by_producer[name]['total_votes'])==int(by_voter[name]['staked'])+int(by_voter[other]['staked'])
        a=rpc('get_account',{'account_name':name});assert not a['privileged']
        required={p['perm_name']:p['required_auth'] for p in a['permissions']}
        assert required['owner']==required['active']==permission(node['public_keys']['account'])
        accounts[name]={'privileged':a['privileged'],'permissions':a['permissions']}
        if name in plan['fault_targets']:
            assert u['ActiveState']=='inactive' and u['MainPID']=='0' and u['UnitFileState']=='disabled'
            assert by_bond[name]['state']==4 and int(by_bond[name]['balance'])==0 and int(by_bond[name]['forfeited'])==int(by_bond[name]['settled'])==100000000
        else:
            assert u['ActiveState']=='active' and int(u['MainPID'])>0 and u['UnitFileState']=='enabled'
            i=rpc('get_info',port=node['rpc_port']);assert i['chain_id']==CHAIN and i['last_irreversible_block_num']>=start['last_irreversible_block_num']-80
            assert int(by_bond[name]['balance'])==100000000 and not int(by_bond[name]['forfeited'])
        votes.append({'producer':name,'self':by_voter[name],'other':by_voter[other],'total':by_producer[name]['total_votes'],'process':u})
    assert {c['owner'] for c in candidates if c['banned']}==set(plan['fault_targets'])
    m=next(x for x in maintenance if x['owner']==plan['maintenance_target']);assert m['state']==2 and 0<int(m['effective_block'])<=start['last_irreversible_block_num']
    proposer=rpc('get_producer_schedule');finalizer=rpc('get_finalizer_info')
    active_p={x['producer_name'] for x in proposer['active']['producers']};active_f={x['description'] for x in finalizer['active_finalizer_policy']['finalizers']}
    pending_p={x['producer_name'] for x in (proposer.get('pending')or{}).get('producers',[])}
    pending_f={x['description'] for x in (finalizer.get('pending_finalizer_policy')or{}).get('finalizers',[])}
    assert len(active_p)==len(active_f)==21 and finalizer['active_finalizer_policy']['threshold']==15
    assert not (set(plan['fault_targets'])|{plan['maintenance_target']}) & (active_p|active_f|pending_p|pending_f)
    cfg=table('bpbondcfg')[0];assert int(cfg['locked'])==balance('eosio.bond')==2500000000
    assert int(cfg['slash_pending'])==0 and int(cfg['slash_settled'])==balance('eosio.bslash')==500000000
    for name,pin in before['code_ABI_pins'].items():
        actual=rpc('get_raw_abi',{'account_name':name});assert actual['code_hash']==pin['code_hash'] and actual['abi_hash']==pin['abi_hash']
    for name,symbol in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]:
        assert rpc('get_currency_stats',{'code':name,'symbol':symbol})==before['token_stats'][name]
    key={n:table(n,'eosio.key','eosio.key') for n in ['wallets','assets','feebuckets','pending','nativefees','relays']}
    assert key['wallets']==before['KEY_snapshot']['wallets'] and key['assets']==before['KEY_snapshot']['assets'] and key['feebuckets']==before['KEY_snapshot']['fees']
    assert all(table('balances','eosio.key',scope)==rows for scope,rows in before['KEY_snapshot']['balances'].items())
    assert not key['pending'] and not key['nativefees'] and len(key['relays'])==5
    assert [(x['bp'],x['endpoint']) for x in key['relays'] if x['enabled']]==[('gptgenesisbp','https://aic-test.matrixai.one/relay')]
    services={u:state(u) for u in ['pureeos-devnet-ordinary-activity-20261105.service','pureeos-devnet-purehistory-api-20261105.service','pureeos-devnet-purehistory-indexer-20261105.service','pureeos-devnet-key-history-20261105.service','pureeos-devnet-relay-20261105.service']}
    assert all(x['ActiveState']=='active' for x in services.values())
    assert state(UNIT)['ActiveState']=='inactive'
    activity=json.loads((C/'activity/STATE.json').read_text());activity={k:activity.get(k) for k in ['status','completed_batches','business_transactions','transfers','ticks','notes','empty_blocks','batch_sizes','UTC']}
    deadline=time.monotonic()+90
    while True:
        end=rpc('get_info')
        if end['last_irreversible_block_num']>start['last_irreversible_block_num']:break
        assert time.monotonic()<deadline,'Irreversible progress deadline';time.sleep(1)
    save('FINAL_AUDIT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'start':start,'end':end,'original_nodes':original_nodes,'original25_PIDs':pids,'old26_archive_disabled':old_archive,'accounts':accounts,'governance':governance,'unchanged_consensus_parameters':previous['consensus_parameters'],'new_self_and_other_votes':votes,'old90_assignments_old_votes_and_old24_bonds_unchanged':True,'30distinct_vote_totals':True,'actual_native_proposers':proposer,'actual_native_finalizers':finalizer,'bonds':bonds,'bond_config':cfg,'maintenance':maintenance,'token_stats':before['token_stats'],'KEY_records_and_sole_founder_relay_unchanged':True,'services':services,'ordinary':activity,'scope':'Read-only feature demonstration audit, no payments or mutation attempts. Actual shared4vCPU/8GB host, no throughput/capacity/zero-rejection qualification.'})
    print('FINAL_READONLY_SIX_BP_DEMO_AUDIT_PASS',flush=True)
except BaseException:
    save('FINAL_AUDIT_FAILURE.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
