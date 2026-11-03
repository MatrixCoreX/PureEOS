"""Authorized whole-chain cutover: retain old data, keys, receipts and volumes."""
from pathlib import Path
import datetime, hashlib, json, os, shutil, subprocess, time, urllib.request

R = Path('/opt/pureeos-keynet-20261103-r1')
O = Path('/opt/pureeos-test45-20261102-r1')
os.umask(0o077)
assert (R/'KEY_ARTIFACTS.json').exists()
assert not (R/'founder').exists(), 'Reconcile a started new chain before freezing again'
assert not (R/'archive/FROZEN.json').exists(), 'Old freeze is already recorded; do not rerun'
expected = json.loads((R/'PLAN.json').read_text())
ports = json.loads((O/'FOUNDER_PORTS.json').read_text())

def rpc(method, data=None):
    req = urllib.request.Request('http://127.0.0.1:' + str(ports['rpc']) + '/v1/chain/' + method,
                                 json.dumps(data or {}).encode(), {'Content-Type':'application/json'})
    with urllib.request.urlopen(req, timeout=15) as response:
        return json.load(response)

info = rpc('get_info')
assert info['chain_id'] == expected['old_chain_id']
units = sorted(p.name for p in Path('/etc/systemd/system').glob('pureeos-test45-*.service'))
active = [u for u in units if subprocess.run(['systemctl','is-active','--quiet',u]).returncode == 0]
names = [e['name'] for e in expected['BPs']]
healthy = ['pureeos-test45-founder-20261102.service'] + ['pureeos-test45-bp-' + n + '-20261102.service' for n in names if n != 'gptgenesisbp']
assert all(u in active for u in healthy)
for n in expected['excluded_old_identities']:
    assert subprocess.run(['systemctl','is-active','--quiet','pureeos-test45-bp-' + n + '-20261102.service']).returncode != 0
activity = 'pureeos-test45-ordinary-activity-20261102.service'
subprocess.run(['systemctl','disable','--now',activity], check=True, capture_output=True)
target = rpc('get_info')['head_block_num']
deadline = time.monotonic() + 300
while rpc('get_info')['last_irreversible_block_num'] < target:
    assert time.monotonic() < deadline, 'Drain/finality did not complete; keep old nodes running'
    time.sleep(1)
archive = R/'archive'
tables = {}
for table in ['producersv2','rotcands','bpmaint','bpbonds','bpbondcfg','votersv2']:
    tables[table] = rpc('get_table_rows',{'json':True,'code':'eosio','scope':'eosio','table':table,'limit':1000})
before = {'chain':rpc('get_info'),'native_finalizers':rpc('get_finalizer_info'),
          'native_proposers':rpc('get_producer_schedule'),'tables':tables,
          'governance':rpc('get_upgrade_state'),'active_old_units':active,
          'drained_through_head':target,'old_data_root':str(O),'old_journals_replayed_on_new_chain':False}
(archive/'BEFORE_FREEZE.json').write_text(json.dumps(before,indent=2)+'\n')
unit_dir = archive/'units'
unit_dir.mkdir(mode=0o700)
for name in units:
    shutil.copy2(Path('/etc/systemd/system')/name,unit_dir/name)
# Stop all old managed services together, as a whole-chain archive operation.
# The ten prior banned/maintenance services were already stopped and stay so.
subprocess.run(['systemctl','disable','--now',*active], check=True, capture_output=True, timeout=240)
containers = ['pureeos-test45-history-' + name for name in ['elasticsearch','mongo','rabbitmq','redis']]
subprocess.run(['docker','update','--restart=no',*containers],check=True,capture_output=True)
subprocess.run(['docker','stop','--time','120',*containers],check=True,capture_output=True,timeout=180)
assert all(subprocess.run(['systemctl','is-active','--quiet',name]).returncode != 0 for name in active)
remaining = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():
        continue
    try:
        command = (proc/'cmdline').read_bytes()
        if str(O).encode() in command and b'/libexec/nodeos' in command:
            remaining.append(int(proc.name))
    except (FileNotFoundError,ProcessLookupError,PermissionError):
        pass
assert not remaining, 'An unmanaged old node remains; do not start new chain'
receipt = {'status':'FROZEN','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'old_chain_id':info['chain_id'],'old_root_retained':str(O),
           'old_units_stopped_disabled':active,'old_history_containers_stopped_retained':containers,
           'ordinary_activity_stopped_disabled':activity,
           'old_volumes_removed':False,'old_keys_and_state_retained_privately':True,
           'previously_stopped_identities_untouched':expected['excluded_old_identities'],
           'new_TPS':False,'new_chain_started':False,
           'unit_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in unit_dir.iterdir()}}
(archive/'FROZEN.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({k:v for k,v in receipt.items() if k != 'unit_hashes'}),flush=True)
