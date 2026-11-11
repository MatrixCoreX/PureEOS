"""Read-only verification of the user's effective-maintenance shutdown request."""
import datetime,hashlib,json,os,subprocess,urllib.request
from pathlib import Path

CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
OPS=Path('/opt/pureeos-testnet-new-ban-20261111-r1')
OUT=Path('/opt/pureeos-testnet-maintenance-stop-20261111-r1')
NATIVE='/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos'
EXPECTED={'montrealmila':'pureeos-bp-demo-montrealmila-20261105.service'}
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def rpc(n,d=None):
    assert n.startswith('get_')
    q=urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+n,json.dumps(d or {}).encode(),{'Content-Type':'application/json'})
    with urllib.request.urlopen(q,timeout=15)as f:return json.load(f)
def state(unit):
    raw=subprocess.check_output(['systemctl','show',unit,'-p','MainPID,ActiveState,SubState,UnitFileState,FragmentPath,NRestarts,Result'],text=True)
    return dict(l.split('=',1)for l in raw.splitlines()if'='in l)
def maint():
    v=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'bpmaint','json':True,'limit':20,'time_limit_ms':1000})
    assert not v.get('more'),'Incomplete maintenance inventory'
    return v['rows']

start=rpc('get_info');assert start['chain_id']==CHAIN
rows=maint();effective={x['owner']:x for x in rows if x['state']==2}
assert set(effective)==set(EXPECTED),'Reconcile any new maintenance target before service changes'
f=rpc('get_finalizer_info');schedule=rpc('get_producer_schedule')
duties={v['description']for v in f['active_finalizer_policy']['finalizers']}
if f.get('pending_finalizer_policy'):duties.update(v['description']for v in f['pending_finalizer_policy']['finalizers'])
for k in['active','pending','proposed']:
    if schedule.get(k):duties.update(v['producer_name']for v in schedule[k]['producers'])
assert not set(effective)&duties
assert len(f['active_finalizer_policy']['finalizers'])==21 and f['active_finalizer_policy']['threshold']==15
targets={n:state(u)for n,u in EXPECTED.items()}
assert all(s['MainPID']=='0'and s['ActiveState']=='inactive'and s['UnitFileState']=='disabled'for s in targets.values())
assert all(int(x['effective_block'])<=start['last_irreversible_block_num']for x in effective.values())
health=json.loads((OPS/'HEALTH_LATEST.json').read_text())
assert health['status']=='OK'and health['running_BP_count']==25
assert not set(effective)&set(health['BP_processes'])
protected={n:v['state']['MainPID']for n,v in health['BP_processes'].items()}
assert all(int(p)>0 for p in protected.values())
history=state('pureeos-devnet-history-20261105.service');assert int(history['MainPID'])>0
physical=[]
for p in Path('/proc').iterdir():
    if not p.name.isdigit():continue
    try:
        if NATIVE in(p/'maps').read_text():physical.append(int(p.name))
    except OSError:pass
expected_pids={int(p)for p in protected.values()}|{int(history['MainPID'])}
assert set(physical)==expected_pids and len(physical)==26,'Unexpected unmanaged native process; investigate before claiming no maintenance process'
# Verify that the automatic guard builds its25-node inventory without the maintenance identity.
old=Path('/opt/pureeos-testnet-resilience-20261110-r1')
inventory=json.loads((old/'RESTORE_NODE_PATHS.json').read_text())
inventory.update({x['name']:x for x in json.loads((old/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
banned=set(json.loads((OPS/'REPLACEMENT_LIVE_BASELINE.json').read_text())['banned'])
inventory={n:v for n,v in inventory.items()if n not in banned}
inventory.update({x['name']:x for x in json.loads((OPS/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
assert len(inventory)==25 and not set(effective)&set(inventory)
end=rpc('get_info');assert end['chain_id']==CHAIN and maint()==rows
result={'status':'PASS_ALREADY_STOPPED_DISABLED_NO_SERVICE_MUTATION','utc':now(),'user_request':'Stop nodes already in effective maintenance to avoid runtime resource use','targets':targets,'maintenance':effective,'absent_from_active_pending_proposed_duties':True,'excluded_from_automatic_recycle_inventory':True,'actual_native_process_PIDs':sorted(physical),'actual_online_BP':25,'actual_online_history':1,'protected_BP_PIDs':protected,'history_PID':history['MainPID'],'native_policy_members':21,'native_policy_threshold':15,'start_info':start,'end_info':end,'already_stopped_before_this_request':True,'service_start_stop_disable_commands_executed':0,'newly_released_process_memory_bytes':0,'configuration_keys_data_and_safety_not_modified':True,'chain_maintenance_votes_bonds_and_authorities_not_modified':True,'no_signing_or_financial_actions':True,'guard_source_sha256':hashlib.sha256((OPS/'recycle_guard.py').read_bytes()).hexdigest()}
os.umask(0o077);OUT.mkdir(mode=0o700,exist_ok=True)
target=OUT/'RESULT.json';assert not target.exists(),'Inspect existing receipt instead of rerunning verification'
target.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(result,ensure_ascii=False))
