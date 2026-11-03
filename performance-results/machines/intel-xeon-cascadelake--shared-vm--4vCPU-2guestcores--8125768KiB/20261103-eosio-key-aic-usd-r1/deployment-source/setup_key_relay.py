"""Deploy the independent BP HTTP relay and verify real AIC/USD operations."""
from pathlib import Path
import hashlib, json, os, pwd, shlex, subprocess, time, urllib.request
from campaign_helpers import R, V, load_chain, action, import_key

os.umask(0o077)
f = load_chain()
f.cleos = R/'native-client/cleos'
assert subprocess.check_output([str(R/'tools/node-current/bin/node'),'--version'],text=True).startswith('v22.')
expected = json.loads((R/'KEY_ARTIFACTS.json').read_text())
assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash'] == expected['files']['eosio.key.wasm']
assert 'eosio.key' in f.governance()['protected_accounts']
assert not (R/'KEY_RELAY_ACCEPTANCE.json').exists(), 'Reconcile existing acceptance; do not repeat demo payments'
assert not (R/'KEY_RELAY_PROGRESS.json').exists(), 'Reconcile any partial progress and canonical receipts before rerunning'
record = {'status':'PREPARING','chain_id':f.rpc('get_info')['chain_id'],'receipts':[],'TPS':'excluded'}
def save():
    (R/'KEY_RELAY_PROGRESS.json').write_text(json.dumps(record,indent=2)+'\n')
def tx(args, failure=None):
    result = f.tx(args,failure=failure)
    if result:
        record['receipts'].append(f.report['confirmed_transactions'][-1])
        save()
    return result
def rows(table, scope='eosio.key'):
    return f.table(table,code='eosio.key',scope=scope)
def native(owner, contract='eosio.token', symbol='AIC'):
    values=f.rpc('get_currency_balance',{'code':contract,'account':owner,'symbol':symbol})
    return int(values[0].split()[0].replace('.','')) if values else 0

for contract,symbol in [('eosio.token','4,AIC'),('keyusdtoken1','4,USD')]:
    acts=[action('buyram','gptgenesisbp',{'payer':'gptgenesisbp','receiver':'eosio.key','quant':'500.0000 AIC'}),
          action('regtoken','gptgenesisbp',{'payer':'gptgenesisbp','token_contract':contract,'token_symbol':symbol,'min_bytes':4096},'eosio.key')]
    tx(['push','transaction',json.dumps({'actions':acts})])
for name in ['gptgenesisbp']:
    tx(['transfer',name,'eosio.key','100.0000 AIC','fee'])
hot=f.key();holder=f.key();recipient=f.key()
for key in [hot,holder,recipient]:
    import_key(f,key)
(R/'private/key-wallet-test-keys.json').write_text(json.dumps({'holder':holder,'recipient':recipient},indent=2)+'\n')
tx(['set','account','permission','gptgenesisbp','keyrelay',hot['Public key'],'active'])
for operation in ['ksend','withdraw','cancel']:
    tx(['set','action','permission','gptgenesisbp','eosio.key',operation,'keyrelay'])
f.action('regrelay',{'bp':'gptgenesisbp','permission':'keyrelay','endpoint':'https://aic-test.matrixai.one/relay'},'gptgenesisbp',contract='eosio.key')
tx(['transfer','gptgenesisbp',holder['Public key'],'50.0000 AIC','--max-key-fee','2.0000 AIC'])
tx(['transfer','gptgenesisbp',holder['Public key'],'50.0000 USD','-c','keyusdtoken1','--max-key-fee','2.0000 AIC'])
try:
    service_user=pwd.getpwnam('pureeoskeyrelay')
except KeyError:
    subprocess.run(['useradd','--system','--user-group','--no-create-home','--shell','/usr/sbin/nologin','pureeoskeyrelay'],check=True)
    service_user=pwd.getpwnam('pureeoskeyrelay')
private=R/'relay-private';private.mkdir(mode=0o700)
state=R/'relay-state';state.mkdir(mode=0o700)
keyfile=private/'hot-key.txt';keyfile.write_text(hot['Private key']+'\n');keyfile.chmod(0o600)
for p in [private,state,keyfile]:
    os.chown(p,service_user.pw_uid,service_user.pw_gid)
unit='pureeos-keynet-relay-20261103.service'
settings={'NODEOS_URL':f.url,'BP_ACCOUNT':'gptgenesisbp','BP_PERMISSION':'keyrelay','RELAY_KEY_FILE':str(keyfile),
          'RELAY_STATE':str(state),'KEY_CODE_HASH':expected['files']['eosio.key.wasm'],'PORT':'8787','RELAY_ALLOWED_TOKENS':'keyusdtoken1'}
text='[Unit]\nDescription=PureEOS key standalone BP relay\nAfter=pureeos-keynet-founder-20261103.service\n[Service]\nType=simple\nUser=pureeoskeyrelay\nGroup=pureeoskeyrelay\nWorkingDirectory='+str(R/'key-service')+'\n'
text+=''.join('Environment='+k+'='+v+'\n' for k,v in settings.items())
text+='ExecStart='+str(R/'tools/node-current/bin/node')+' --max-old-space-size=128 '+str(R/'key-service/relayer/server.mjs')+'\nRestart=on-failure\nRestartSec=5\nNoNewPrivileges=true\nUMask=0077\n[Install]\nWantedBy=multi-user.target\n'
Path('/etc/systemd/system',unit).write_text(text)
subprocess.run(['systemctl','daemon-reload'],check=True)
subprocess.run(['systemctl','enable','--now',unit],check=True,capture_output=True)
endpoint='http://127.0.0.1:8787'
deadline=time.monotonic()+60
while True:
    try:
        with urllib.request.urlopen(endpoint+'/health',timeout=3) as response:
            json.load(response)
        break
    except OSError:
        assert time.monotonic()<deadline,'Relay readiness timeout'
        time.sleep(.5)
def relay_send(to,quantity,contract='eosio.token'):
    prepared=json.loads(f.run(['key','prepare',holder['Public key'],to,quantity,'-c',contract,'--bp','gptgenesisbp','--max-fee','2.0000 AIC']))
    signed=json.loads(f.run(['key','sign',json.dumps(prepared)]))
    start=f.rpc('get_info')['last_irreversible_block_num']
    reply=json.loads(f.run(['key','submit',json.dumps(signed),'--relay-url',endpoint,'--allow-http-for-test']))
    receipt=f.wait_transaction(reply['transaction_id'],start,timeout=180)
    end=time.monotonic()+90
    while True:
        with urllib.request.urlopen(endpoint+'/status/'+reply['intent'],timeout=5)as response:
            status=json.load(response)
        if status['status']=='finalized':
            break
        assert time.monotonic()<end,'Relay did not reconcile irreversible receipt'
        time.sleep(2)
    assert status['confirmation']['block_id']==receipt['block_id']
    again=json.loads(f.run(['key','submit',json.dumps(signed),'--relay-url',endpoint,'--allow-http-for-test']))
    assert again['transaction_id']==reply['transaction_id']and again['status']=='finalized'
    record['receipts'].append(receipt);save()
    return receipt
relay_send(recipient['Public key'],'1.2500 USD','keyusdtoken1')
relay_send(recipient['Public key'],'2.0000 AIC')
before_wallet=rows('wallets');before_assets=rows('assets')
prepared=json.loads(f.run(['key','prepare',holder['Public key'],'civicusertwo','0.5000 USD','-c','keyusdtoken1','--self','--payer','civicuserone','--max-fee','2.0000 AIC']))
signed=json.loads(f.run(['key','sign',json.dumps(prepared)]))
withdrawal_start=f.rpc('get_info')['last_irreversible_block_num']
tx(['key','push',json.dumps(signed),'-p','civicuserone@active'],failure='recipient must open')
assert rows('wallets')==before_wallet and rows('assets')==before_assets and native('civicusertwo','keyusdtoken1','USD')==0
f.action('open',{'owner':'civicusertwo','symbol':'4,USD','ram_payer':'civicusertwo'},'civicusertwo',contract='keyusdtoken1')
ram_before=int(f.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
# Retry precisely the owner-approved native ID after opening, never mint a
# replacement authorization for the earlier rejected payment.
try:
    tx(['key','push',json.dumps(signed),'-p','civicuserone@active'])
except RuntimeError as error:
    if 'duplicate transaction' not in str(error):
        raise
    record['receipts'].append(f.wait_transaction(prepared['transaction_id'],withdrawal_start,timeout=180))
assert native('civicusertwo','keyusdtoken1','USD')==5000
assert int(f.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==ram_before
earned=next(int(r['relay_earned'])for r in rows('feebuckets')if r['owner']=='gptgenesisbp')
assert earned>0
quantity=str(earned//10000)+'.'+str(earned%10000).zfill(4)+' AIC'
f.action('claim',{'bp':'gptgenesisbp','to':'gptgenesisbp','quantity':quantity,'max_fee':'2.0000 AIC'},'gptgenesisbp',contract='eosio.key')
assert next(int(r['relay_earned'])for r in rows('feebuckets')if r['owner']=='gptgenesisbp')==0
record.update(status='PASS',unit=unit,standalone_HTTP_port=8787,public_endpoint='https://aic-test.matrixai.one/relay',
    holder_public_key=holder['Public key'],recipient_public_key=recipient['Public key'],
    whitelist=['eosio.token::4,AIC','keyusdtoken1::4,USD'],wallet_WASM=expected['files']['eosio.key.wasm'],
    unopened_USD_withdrawal_rejected=True,opened_USD_withdrawal_extra_custody_RAM=0,relay_earned_atoms=earned,
    prior_relay_income_claim_verified=True,duplicate_HTTP_submission_same_native_ID=True,
    native_cleos_SHA256=hashlib.sha256((R/'native-client/cleos-key').read_bytes()).hexdigest())
(R/'KEY_RELAY_ACCEPTANCE.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'status':'PASS','chain_id':record['chain_id'],'holder_public_key':record['holder_public_key'],'recipient_public_key':record['recipient_public_key'],'relay_endpoint':record['public_endpoint']}),flush=True)
