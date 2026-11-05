"""Remote, read-only verification; never starts a service or submits actions."""
from pathlib import Path
import datetime, hashlib, importlib.util, json, subprocess, time, urllib.request
ROOT=Path('/opt/pureeos-system-signup-fix-20261105-r2')
spec=importlib.util.spec_from_file_location('window', ROOT/'window.py')
wmod=importlib.util.module_from_spec(spec);spec.loader.exec_module(wmod)
def complete_tables():
    names={x['name'] for x in wmod.rpc('get_abi',{'account_name':'eosio'})['abi']['tables']};out={}
    for name in ['producers','producersv2','voters','votersv2','votepolicy','bpbonds','bpbondcfg','bpmaint','rotcands']:
        if name not in names:continue
        rows=[];lower='';seen=set()
        for page in range(128):
            value=wmod.rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':name,'json':True,'limit':1000,'lower_bound':lower})
            rows.extend(value['rows'])
            if not value['more']:break
            next_key=str(value.get('next_key',''));assert next_key and next_key not in seen,'Incomplete/repeated table cursor '+name
            seen.add(next_key);lower=next_key
        else:raise AssertionError('Table pagination bound '+name)
        out[name]=rows
    return out
wmod.tables=complete_tables
w=json.loads((ROOT/'WINDOW.json').read_text());assert w['status']=='RESTORED_VERIFIED'
before=wmod.info();time.sleep(15);after=wmod.info()
assert after['last_irreversible_block_num']>before['last_irreversible_block_num']
nodes={}
for node in w['nodes']:
    p=wmod.properties(node['unit']);assert p['ActiveState']=='active' and int(p['MainPID'])>0
    pid=int(p['MainPID']);nodes[node['unit']]=pid
    assert str(wmod.R/'pureeos-test-1.0.0-test.1/libexec/nodeos') in (Path('/proc')/str(pid)/'maps').read_text()
    assert wmod.sha(Path(node['config_dir'])/'config.ini')==node['config_sha256']
    assert wmod.sha(node['unit_file'])==node['unit_sha256']
    if node['unit']==wmod.GERMANY:assert pid==node['pid']
assert all(wmod.properties(u)['ActiveState']=='active' for u in w['services'])
containers={}
for c in w['containers']:
    row=json.loads(subprocess.check_output(['docker','inspect',c['id']],text=True))[0]
    assert row['Id']==c['id'] and row['Image']==c['image'] and row['State']['Running']
    containers[c['name']]=row['State'].get('Health',{}).get('Status','running-no-healthcheck')
state=wmod.snapshot();policy=state['finalizers']['active_finalizer_policy']
assert len(policy['finalizers'])==21 and policy['threshold']==15
assert 'germanycrest' not in [x['description'] for x in policy['finalizers']]
saved=json.loads((ROOT/'AFTER_CHAIN_BEFORE_ACTIVITY.json').read_text())
assert wmod.invariant(saved)==json.loads((ROOT/'BEFORE_INVARIANTS.json').read_text())
assert wmod.invariant(state)==wmod.invariant(saved)
assert wmod.sha(wmod.R/'pureeos-test-1.0.0-test.1/libexec/nodeos')==w['original_node_sha256']
activity=json.loads((wmod.R/'activity/STATE.json').read_text())
caps=wmod.rpc('get_consensus_parameters');old=json.loads(Path('/opt/pureeos-dev-proposals-20261104-r1/CONSENSUS_BEFORE.json').read_text());assert caps==old
result={'status':'PASS','UTC':wmod.now(),'chain_id':after['chain_id'],'head':after['head_block_num'],'LIB':after['last_irreversible_block_num'],
 'LIB_progress':after['last_irreversible_block_num']-before['last_irreversible_block_num'],'node_count':len(nodes),'node_pids':nodes,
 'original_binary_sha256':w['original_node_sha256'],'all_configs_and_units_unchanged':True,'Germany_original_PID_preserved':True,
 'frozen_governance_economics_and_90_balances_matched_before_business_resume':True,'consensus_caps_unchanged':True,
 'native_finalizers':21,'native_threshold':15,'cap_proposal3_pending_vetoes':state['governance']['effective_veto_count'],
 'token_stats':state['stats'],'banned':['germanycrest'],'services_active':w['services'],'containers':containers,
 'ordinary_progress':{k:v for k,v in activity.items() if type(v)is int},'ordinary_has_pending':activity.get('pending') is not None,
 'candidate_installed':False,'feature_activation_or_live_governance_transactions':False}
wmod.save('POST_RESTORE_VERIFICATION.json',result);print(json.dumps(result))
