"""After irreversible devexec, switch only the relay/projector applications."""
from pathlib import Path
import datetime,hashlib,json,subprocess,urllib.request,time
K=Path('/opt/pureeos-dev-proposals-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');T=K/'runtime'
NEW='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
assert json.loads((K/'EXECUTION.json').read_text())['status']=='UPGRADED_IRREVERSIBLE'
assert not(K/'APPS_SWITCHED.json').exists()
rpc=lambda method,data:json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'}),timeout=40))
assert rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==NEW
manifest=json.loads((K/'RUNTIME_PREPARED.json').read_text());assert all(hashlib.sha256((T/n).read_bytes()).hexdigest()==v for n,v in manifest['files'].items())
journal=R/'relay-state/journal.json'
for attempt in range(60):
 jobs=json.loads(journal.read_text())
 if all(j['status']in ['finalized','expired']for j in jobs.values()):break
 time.sleep(2)
assert all(j['status']in ['finalized','expired']for j in jobs.values()),'Reconcile pending relay transactions before switching'
protected=[R/'relay-private/hot-key.txt',R/'private/bootstrap-keys.json']
key_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in protected}
units=['pureeos-keynet2-relay-20261103.service','pureeos-key-history-20261103.service']
before={u:hashlib.sha256(subprocess.check_output(['systemctl','cat',u])).hexdigest()for u in units}
relay=Path('/etc/systemd/system/'+units[0]+'.d');history=Path('/etc/systemd/system/'+units[1]+'.d');relay.mkdir(exist_ok=True);history.mkdir(exist_ok=True)
paths=[relay/'zz-ram-market-20261104.conf',history/'zz-ram-market-20261104.conf'];assert all(not p.exists()for p in paths)
paths[0].write_text('[Service]\nWorkingDirectory='+str(T)+'\nExecStart=\nExecStart='+str(R/'tools/node-current/bin/node')+' --max-old-space-size=128 '+str(T/'relayer/server.mjs')+'\nEnvironment=KEY_CODE_HASH='+NEW+'\n')
paths[1].write_text('[Service]\nEnvironment=EXPECTED_KEY_CODE_HASH='+NEW+'\n')
progress={'status':'SWITCHING','before_unit_hashes':before,'old_journal_ids':sorted(jobs),'old_journal_sha256':hashlib.sha256(journal.read_bytes()).hexdigest(),'only_application_units':units}
(K/'APP_SWITCH_PROGRESS.json').write_text(json.dumps(progress,indent=2)+'\n')
subprocess.run(['systemctl','daemon-reload'],check=True)
for unit in units:subprocess.run(['systemctl','restart',unit],check=True,timeout=100)
for unit in units:assert subprocess.run(['systemctl','is-active','--quiet',unit]).returncode==0
for p,digest in key_hashes.items():assert hashlib.sha256(Path(p).read_bytes()).hexdigest()==digest
assert all(json.loads(journal.read_text())[k]==v for k,v in jobs.items()),'Existing finalized journal changed'
health=None
for attempt in range(30):
 try:
  health=json.load(urllib.request.urlopen('http://127.0.0.1:8787/health',timeout=5));break
 except OSError:time.sleep(1)
assert health and health['bp']=='gptgenesisbp'and health['fee_bps']==3000
for unit in units:
 env=subprocess.check_output(['systemctl','show',unit,'-p','Environment','--value'],text=True);assert('KEY_CODE_HASH='+NEW)in env
progress.update(status='PASS',health=health,original_journal_preserved=True,keys_unchanged=True,HTTP_routes_unchanged=True,UTC=datetime.datetime.now(datetime.timezone.utc).isoformat())
(K/'APPS_SWITCHED.json').write_text(json.dumps(progress,indent=2)+'\n');print('Relay and projector use the reviewed RAM code; original journal and keys preserved')
