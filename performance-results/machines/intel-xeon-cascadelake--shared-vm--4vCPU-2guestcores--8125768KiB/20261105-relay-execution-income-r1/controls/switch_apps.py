"""Switch only qualified application pins after irreversible native execution."""
from pathlib import Path
import datetime,hashlib,json,os,subprocess,time,urllib.request
K=Path('/opt/pureeos-relay-execution-income-20261105-r1')
C=Path('/opt/pureeos-devnet-20261105-r1');S=K/'runtime'
NEW='1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d'
assert not(K/'APPS_SWITCHED.json').exists()
assert json.loads((K/'EXECUTION.json').read_text())['status']=='UPGRADED_IRREVERSIBLE'
assert json.loads((K/'RUNTIME_TEST_RESULT.json').read_text())['status']=='PASS'
def rpc(method,data):
    q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,json.dumps(data).encode(),{'Content-Type':'application/json'})
    with urllib.request.urlopen(q,timeout=15)as r:return json.load(r)
assert rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==NEW
manifest=json.loads((K/'RUNTIME_SOURCE.json').read_text())
for n,h in manifest['files'].items():assert hashlib.sha256((S/n).read_bytes()).hexdigest()==h,n
journal=C/'relay-state/journal.json';deadline=time.monotonic()+180
while True:
    jobs=json.loads(journal.read_text())
    if all(j['status']in ['finalized','expired']for j in jobs.values()):break
    assert time.monotonic()<deadline,'Pending relay packets require canonical reconciliation'
    time.sleep(2)
protected=[C/'relay-private/hot-key.txt',C/'private/bootstrap-keys.json',C/'private/relay-key.json']
key_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in protected}
units=['pureeos-devnet-relay-20261105.service','pureeos-devnet-key-history-20261105.service']
before={u:hashlib.sha256(subprocess.check_output(['systemctl','cat',u])).hexdigest()for u in units}
for i,u in enumerate(units):
    d=Path('/etc/systemd/system')/(u+'.d');d.mkdir(exist_ok=True)
    p=d/'zz-relay-income-20261105.conf';assert not p.exists()
    text='[Service]\n'
    if i==0:text+='WorkingDirectory='+str(S)+'\nExecStart=\nExecStart='+str(C/'tools/node-current/bin/node')+' --max-old-space-size=128 '+str(S/'relayer/server.mjs')+'\nEnvironment=KEY_CODE_HASH='+NEW+'\n'
    else:text+='Environment=EXPECTED_KEY_CODE_HASH='+NEW+'\n'
    p.write_text(text)
progress={'status':'SWITCHING','before_unit_hashes':before,'original_journal_sha256':hashlib.sha256(journal.read_bytes()).hexdigest(),'original_journal_ids':sorted(jobs),'units':units}
(K/'APP_SWITCH_PROGRESS.json').write_text(json.dumps(progress,indent=2)+'\n')
subprocess.run(['systemctl','daemon-reload'],check=True)
for u in units:subprocess.run(['systemctl','restart',u],check=True,timeout=90)
for u in units:assert subprocess.run(['systemctl','is-active','--quiet',u]).returncode==0
for p,h in key_hashes.items():assert hashlib.sha256(Path(p).read_bytes()).hexdigest()==h
current=json.loads(journal.read_text());assert all(current[k]==v for k,v in jobs.items())
deadline=time.monotonic()+90
while True:
    try:
        with urllib.request.urlopen('http://127.0.0.1:8787/health',timeout=5)as r:health=json.load(r)
        assert health['bp']=='gptgenesisbp'and health['fee_bps']==3000;break
    except OSError:assert time.monotonic()<deadline;time.sleep(1)
for u in units:
    env=subprocess.check_output(['systemctl','show',u,'--property=Environment','--value'],text=True)
    assert 'KEY_CODE_HASH='+NEW in env
progress.update(status='PASS',UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),health=health,existing_keys_and_journal_preserved=True,sole_HTTP_unchanged=True)
(K/'APPS_SWITCHED.json').write_text(json.dumps(progress,indent=2)+'\n')
print('QUALIFIED_RELAY_AND_PROJECTION_PINS_SWITCHED')
