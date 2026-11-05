"""One founder relay; four inactive comparison entries, no additional hotkeys/permissions/HTTP services."""
from pathlib import Path
import datetime,hashlib,json,os,pwd,shlex,subprocess,time,urllib.request
from campaign_helpers import R,load_chain,import_key
os.umask(0o077);f=load_chain();assert json.loads((R/'CREATION_EXECUTION_RESULT_r2.json').read_text())['status']=='FOUR_CREATIONS_DEPLOYED_IRREVERSIBLE'
assert json.loads((R/'KEY_SERVICE_TEST_RESULT_r2.json').read_text())['status']=='PASS'
assert not(R/'KEY_SERVICE_PROGRESS.json').exists(),'Reconcile exact native actions before resuming'
code='7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788';assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==code
state={'status':'CONFIGURING','chain_id':f.rpc('get_info')['chain_id'],'confirmed':[]}
def save():(R/'KEY_SERVICE_PROGRESS.json').write_text(json.dumps(state,indent=2)+'\n')
save();hot=f.key();(R/'private/relay-key.json').write_text(json.dumps(hot));(R/'private/relay-key.json').chmod(0o600)
f.tx(['set','account','permission','gptgenesisbp','keyrelay',hot['Public key'],'active'])
for op in ['ksend','withdraw','cancel']:f.tx(['set','action','permission','gptgenesisbp','eosio.key',op,'keyrelay'])
rows=[('canadamilaai',1000,''),('anthropiclab',1500,''),('deepseekcore',2000,''),('singaporentu',2500,''),('gptgenesisbp',3000,'https://aic-test.matrixai.one/relay')]
for name,rate,endpoint in rows:
 f.action('regrelay',{'bp':name,'permission':'keyrelay','endpoint':endpoint},name,contract='eosio.key')
 f.action('setrate',{'bp':name,'fee_bps':rate},name,contract='eosio.key');state['confirmed']=f.report['confirmed_transactions'];save()
# Public code paths may be traversed; founder/BP/config/wallet/private trees stay protected.
R.chmod(0o755);S=R/'key-service'
for p in[S,*(S/'sdk').rglob('*'),S/'sdk',*(S/'relayer').rglob('*'),S/'relayer']:
 if not p.is_symlink():p.chmod(0o755 if p.is_dir()else 0o644)
user=pwd.getpwnam('pureeoskeyrelay');private=R/'relay-private';journal=R/'relay-state';private.mkdir(mode=0o700);journal.mkdir(mode=0o700)
keyfile=private/'hot-key.txt';keyfile.write_text(hot['Private key']+'\n');keyfile.chmod(0o600)
for p in[private,journal,keyfile]:os.chown(p,user.pw_uid,user.pw_gid)
settings={'NODEOS_URL':'http://127.0.0.1:8888','NODEOS_READ_URL':'http://127.0.0.1:8890','BP_ACCOUNT':'gptgenesisbp','BP_PERMISSION':'keyrelay','RELAY_KEY_FILE':str(keyfile),'RELAY_STATE':str(journal),'KEY_CODE_HASH':code,'PORT':'8787','RELAY_ALLOWED_TOKENS':'usd,cny'}
unit='pureeos-devnet-relay-20261105.service';text='[Unit]\nDescription=PureEOS TEST sole founder key relay\nAfter=pureeos-devnet-founder-20261105.service\n[Service]\nType=simple\nUser=pureeoskeyrelay\nGroup=pureeoskeyrelay\nWorkingDirectory='+str(S)+'\n'+''.join('Environment='+k+'='+v+'\n'for k,v in settings.items())+'ExecStart='+str(R/'tools/node-current/bin/node')+' --max-old-space-size=128 '+str(S/'relayer/server.mjs')+'\nRestart=on-failure\nRestartSec=5\nNoNewPrivileges=true\nUMask=0077\nMemoryMax=256M\nCPUQuota=30%\n[Install]\nWantedBy=multi-user.target\n'
Path('/etc/systemd/system',unit).write_text(text);subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','enable','--now',unit],check=True,capture_output=True)
deadline=time.monotonic()+90
while True:
 try:
  with urllib.request.urlopen('http://127.0.0.1:8787/health',timeout=5)as r:health=json.load(r)
  assert health['bp']=='gptgenesisbp'and health['fee_bps']==3000;break
 except Exception:assert time.monotonic()<deadline;time.sleep(1)
actual=f.table('relays',code='eosio.key',scope='eosio.key');assert len(actual)==5 and sum(bool(x['enabled'])for x in actual)==1
for name,_,endpoint in rows:
 row=next(x for x in actual if x['bp']==name);assert row['endpoint']==endpoint and bool(row['enabled'])==bool(endpoint)
 if not endpoint:assert not any(x['perm_name']=='keyrelay'for x in f.rpc('get_account',{'account_name':name})['permissions'])
state.update(status='PASS',unit=unit,health=health,registrations=actual,rates=f.table('relayrates',code='eosio.key',scope='eosio.key'),sole_HTTP_relay=True,comparison_hotkeys_permissions_services=0)
save();(R/'KEY_SERVICE_RESULT.json').write_text(json.dumps(state,indent=2)+'\n');print('SOLE_FOUNDER_RELAY_AND_FOUR_INACTIVE_RATES_PASS',flush=True)
