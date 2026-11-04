"""After routing tests, switch only the existing relay's read-RPC/runtime."""
from pathlib import Path
import datetime,hashlib,json,subprocess,time,urllib.request
K=Path('/opt/pureeos-dev-proposals-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');S=K/'runtime-rpcsplit';NEW='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
qualified=json.loads((K/'RPC_SPLIT_TEST_RESULT.json').read_text());assert qualified['status']=='PASS'
for name,digest in qualified['files'].items():assert hashlib.sha256((S/name).read_bytes()).hexdigest()==digest
assert not(K/'RPC_SPLIT_SWITCHED.json').exists()
probe="""import fs from 'node:fs';import {Relay} from './runtime-rpcsplit/relayer/server.mjs';
const k='/opt/pureeos-dev-proposals-20261104-r1',r='/opt/pureeos-keynet-20261103-r2';
const relay=new Relay({endpoint:'http://127.0.0.1:8888',readEndpoint:'http://127.0.0.1:8890',bp:'gptgenesisbp',permission:'keyrelay',key:fs.readFileSync(r+'/relay-private/hot-key.txt','utf8').trim(),expectedHash:'bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1',journal:{jobs:{}},allowedTokens:['keyusdtoken1']});
await relay.profile();fs.writeFileSync(k+'/RPC_SPLIT_LIVE_PROFILE.json',JSON.stringify({status:'PASS',chain_id:relay.client.config.chain_id,read:'http://127.0.0.1:8890',writer:'http://127.0.0.1:8888',writer_code_pin_checked:true},null,2)+'\\n');"""
(K/'probe-rpc-split.mjs').write_text(probe)
subprocess.run([str(R/'tools/node-current/bin/node'),str(K/'probe-rpc-split.mjs')],check=True)
journal=R/'relay-state/journal.json';jobs=json.loads(journal.read_text());assert all(j['status']in ['finalized','expired']for j in jobs.values())
unit='pureeos-keynet2-relay-20261103.service';drop=Path('/etc/systemd/system')/(unit+'.d')/'zzz-relay-read-rpc-20261104.conf';assert not drop.exists()
drop.write_text('[Service]\nWorkingDirectory='+str(S)+'\nExecStart=\nExecStart='+str(R/'tools/node-current/bin/node')+' --max-old-space-size=128 '+str(S/'relayer/server.mjs')+'\nEnvironment=NODEOS_READ_URL=http://127.0.0.1:8890\n')
keyhash=hashlib.sha256((R/'relay-private/hot-key.txt').read_bytes()).hexdigest()
subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','restart',unit],check=True,timeout=90)
health=None
for attempt in range(30):
 try:health=json.load(urllib.request.urlopen('http://127.0.0.1:8787/health',timeout=5));break
 except OSError:time.sleep(1)
assert health and health['bp']=='gptgenesisbp'and health['fee_bps']==3000
assert hashlib.sha256((R/'relay-private/hot-key.txt').read_bytes()).hexdigest()==keyhash
assert all(json.loads(journal.read_text())[key]==value for key,value in jobs.items())
result={'status':'PASS','runtime':str(S),'source_sha256':qualified['source_sha256'],'read_RPC':'http://127.0.0.1:8890','broadcast_RPC':'http://127.0.0.1:8888',
 'same_chain_and_writer_code_verified':True,'same_hot_key_permission_HTTP_endpoint_and_journal':True,'node_processes_or_caps_changed':False,'unit_dropin':str(drop),
 'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()}
(K/'RPC_SPLIT_SWITCHED.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
