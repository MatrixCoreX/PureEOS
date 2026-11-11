"""One new independent relay2, authorized by existing anthropiclab active only.
Never changes owner/active, production/finalizer/peer keys or native node services.
"""
from pathlib import Path
import json,subprocess,os,datetime,urllib.request,time,shutil
import ops_common as o
from admin_session import AdminSession
R=o.R;G=Path('/opt/pureeos-devnet-20261105-r1');BP='anthropiclab';endpoint='https://relay2.matrixai.one';permission='keyrelay'
os.umask(0o077)
assert not(R/'REGISTER_STARTED.json').exists();o.guard();o.require_codes(o.NEW);o.authority_guard();assert json.loads((R/'TLS_RESULT.json').read_text())['status']=='PASS'
baseline=json.loads((R/'BASELINE.json').read_text());before=baseline['BP_account'];assert o.rpc('get_account',{'account_name':BP})['permissions']==before['permissions'];assert not any(p['perm_name']==permission for p in before['permissions'])
o.save(R/'REGISTER_STARTED.json',{'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'BP':BP,'permission':permission,'endpoint':endpoint,'fee_bps':1000,'scope':__doc__})
private=R/'relay2-private';private.mkdir(mode=0o700)
with AdminSession()as signer:
 hot=signer.key();public=hot['Public key'];assert public!=next(x for x in baseline['records']['producersv2']if x['owner']==BP)['producer_key'];assert all(public!=k['key']for p in before['permissions']for k in p['required_auth']['keys'])
 p=private/'relay.key';p.write_text(hot['Private key']+'\n');p.chmod(0o600);hot=None
 auth=[{'actor':BP,'permission':'active'}];actions=[{'account':'eosio','name':'updateauth','authorization':auth,'data':{'account':BP,'permission':permission,'parent':'active','auth':{'threshold':1,'keys':[{'key':public,'weight':1}],'accounts':[],'waits':[]}}}]
 for action in ['ksend','withdraw','cancel']:actions.append({'account':'eosio','name':'linkauth','authorization':auth,'data':{'account':BP,'code':'eosio.key','type':action,'requirement':permission}})
 actions.extend([{'account':'eosio.key','name':'regrelay','authorization':auth,'data':{'bp':BP,'permission':permission,'endpoint':endpoint}},{'account':'eosio.key','name':'setrate','authorization':auth,'data':{'bp':BP,'fee_bps':1000}}])
 receipt=signer.send('register-relay2',actions);after=o.rpc('get_account',{'account_name':BP});assert [(x['perm_name'],x['parent'],x['required_auth'])for x in after['permissions']if x['perm_name']in['owner','active']]==[(x['perm_name'],x['parent'],x['required_auth'])for x in before['permissions']]
 child=next(x for x in after['permissions']if x['perm_name']==permission);assert child['parent']=='active'and child['required_auth']['threshold']==1 and len(child['required_auth']['keys'])==1
 row=next(x for x in o.rows('eosio.key','eosio.key','relays')if x['bp']==BP);rate=next(x for x in o.rows('eosio.key','eosio.key','relayrates')if x['bp']==BP);assert row['enabled']and row['endpoint']==endpoint and int(rate['fee_bps'])==1000
 o.save(R/'REGISTER_RESULT.json',{'status':'REGISTERED_IRREVERSIBLE','BP':BP,'permission':permission,'endpoint':endpoint,'fee_bps':1000,'public_hot_key':public,'canonical_receipt':receipt['canonical_receipt'],'owner_active_unchanged':True,'child_permission':child,'relay':row,'rate':rate})
# Reuse exact qualified runtime; new state/key/service are isolated from first relay.
S=R/'relay2-runtime';assert not S.exists();old=Path('/opt/pureeos-key-native-ram-20261111-r1/relay-runtime');manifest=json.loads(Path('/opt/pureeos-key-native-ram-20261111-r1/RUNTIME_SOURCE.json').read_text())['files']
for n,h in manifest.items():
 assert o.sha((old/n).read_bytes())==h;p=S/n;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(old/n,p)
(S/'node_modules').symlink_to(old/'node_modules',target_is_directory=True)
state=R/'relay2-state';state.mkdir(mode=0o700)
unit='pureeos-relay2-anthropiclab-20261111.service';path=Path('/etc/systemd/system')/unit;assert not path.exists()
path.write_text(f'''[Unit]
Description=PureEOS TEST second BP relay anthropiclab10percent
After=network-online.target
[Service]
Type=simple
WorkingDirectory={S}
ExecStart={G}/tools/node-current/bin/node --max-old-space-size=96 {S}/relayer/server.mjs
Environment=NODEOS_URL=http://127.0.0.1:8888
Environment=NODEOS_READ_URL=http://127.0.0.1:8890
Environment=BP_ACCOUNT={BP}
Environment=BP_PERMISSION={permission}
Environment=RELAY_KEY_FILE={private}/relay.key
Environment=RELAY_STATE={state}
Environment=RELAY_ALLOWED_TOKENS=usd,cny
Environment=KEY_CODE_HASH={o.NEW}
Environment=PORT=8788
Nice=10
CPUWeight=50
MemoryHigh=128M
MemoryMax=192M
MemorySwapMax=64M
LimitCORE=0
Restart=on-failure
RestartSec=10
[Install]
WantedBy=multi-user.target
''')
subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','enable','--now',unit],check=True,timeout=60)
deadline=time.monotonic()+60
while True:
 try:
  with urllib.request.urlopen('http://127.0.0.1:8788/health',timeout=5)as f:health=json.load(f)
  assert health['bp']==BP and health['permission']==permission and health['fee_bps']==1000;break
 except(OSError,AssertionError):assert time.monotonic()<deadline;time.sleep(1)
with urllib.request.urlopen(endpoint+'/health',timeout=20)as f:public=json.load(f)
assert public==health;o.guard();o.require_codes(o.NEW);o.authority_guard()
original=next(x for x in baseline['relays']if x['bp']=='gptgenesisbp');assert next(x for x in o.rows('eosio.key','eosio.key','relays')if x['bp']=='gptgenesisbp')==original
assert next(x for x in o.rows('eosio.key','eosio.key','relayrates')if x['bp']=='gptgenesisbp')==next(x for x in baseline['rates']if x['bp']=='gptgenesisbp')
o.save(R/'RELAY2_SERVICE_RESULT.json',{'status':'PASS_NEW_SERVICE_AND_PUBLIC_HTTPS_HEALTH','endpoint':endpoint,'health':public,'unit':unit,'port':8788,'runtime_source_files':manifest,'existing_founder30percent_unchanged':True,'original_native26_processes_unchanged':True,'new_hot_key_only_in_new_service_private_file':True,'admin_signer_closed':True})
print('RELAY2_REGISTERED_IRREVERSIBLE_AND_PUBLIC_HTTPS_HEALTH_PASS')
