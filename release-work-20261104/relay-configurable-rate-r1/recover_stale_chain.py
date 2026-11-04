"""Bounded recovery of the user-authorized full halt, preserving native safety."""
from pathlib import Path
import datetime, hashlib, json, shutil, subprocess, time, urllib.request
K=Path('/opt/pureeos-key-rate-20261104-r1')
R=Path('/opt/pureeos-keynet-20261103-r2')
unit='pureeos-keynet2-bp-anthropiclab-20261103.service'
bp='anthropiclab'
def rpc(method,data={},port=8888):
    q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/{method}',data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
    return json.load(urllib.request.urlopen(q,timeout=10))
base=rpc('get_info')
assert base['chain_id']=='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
candidates=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'rotcands','json':True,'limit':1000})['rows']
banned={x['owner'] for x in candidates if x['banned']}
assert banned=={'germanycrest'}
inventory=json.loads((K/'RESTART_HEAD_INVENTORY.json').read_text())['nodes']
verified=[]
for node in inventory:
    if node['unit']=='pureeos-keynet2-bp-germanycrest-20261103.service': continue
    info=rpc('get_info',port=node['port'])
    assert info['head_block_id']==base['head_block_id'] and info['last_irreversible_block_id']==base['last_irreversible_block_id']
    verified.append({'unit':node['unit'],'head_id':info['head_block_id'],'LIB_id':info['last_irreversible_block_id']})
assert len(verified)==25
schedule=rpc('get_producer_schedule')['active']['producers']
names=[p['producer_name'] for p in schedule]
assert bp in names and len(names)==21
conf=R/'bps/anthropiclab/config/config.ini'
original=conf.read_bytes()
assert b'enable-stale-production' not in original
folder=K/'private-runtime-backups';folder.mkdir(mode=0o700,exist_ok=True)
backup=folder/'anthropiclab-before-stale-recovery.ini'
assert not backup.exists()
shutil.copyfile(conf,backup);backup.chmod(0o600)
record={'status':'RECOVERING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'unit':unit,'original_config_sha256':hashlib.sha256(original).hexdigest(),
        'verified_24_eligible_BP_and_history':verified,'banned_node_excluded_from_recovery':sorted(banned),
        'starting_head':base,'native_schedule':names,'config_restored':False,
        'scope':'One currently active BP proposes from the common durable branch; no key/safety/quorum/cap changes. Native21/15 remains required.'}
path=K/'STALE_RECOVERY.json'
path.write_text(json.dumps(record,indent=2)+'\n')
try:
    # The native implicit vote timeout remains6s. Start during this BP's own
    # normal12-slot window so it can propose promptly and receive real votes.
    target=names.index(bp);deadline=time.monotonic()+150
    while True:
        slot=int((time.time()-946684800)*2)+1
        if (slot//12)%len(names)==target and slot%12<=3: break
        assert time.monotonic()<deadline
        time.sleep(.2)
    conf.write_bytes(original+b'\nenable-stale-production = true\n')
    subprocess.run(['systemctl','restart',unit],check=True,timeout=120)
    deadline=time.monotonic()+90
    while True:
        info=rpc('get_info')
        if info['last_irreversible_block_num']>=base['head_block_num']+3:
            assert abs((datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)-datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds())<20
            record['recovered_info']=info
            break
        assert time.monotonic()<deadline,'Native quorum did not recover; safety is not bypassed'
        time.sleep(1)
    record['status']='RECOVERED'
finally:
    conf.write_bytes(original)
    assert hashlib.sha256(conf.read_bytes()).hexdigest()==record['original_config_sha256']
    record['config_restored']=True
    subprocess.run(['systemctl','restart',unit],check=True,timeout=120)
    path.write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'status':record['status'],'head':record['recovered_info']['head_block_num'],
                  'LIB':record['recovered_info']['last_irreversible_block_num'],'original_config_restored':True}),flush=True)
