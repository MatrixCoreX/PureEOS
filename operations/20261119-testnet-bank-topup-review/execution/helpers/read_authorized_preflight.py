from pathlib import Path
import json,sys,hashlib
sys.path.insert(0,'/opt/pureeos-testnet-stall-recovery-20261116-r1');import stability_guard as g
info=g.rpc('get_info',port=8890);assert info['chain_id']==g.CHAIN
pins=json.loads(Path('/opt/pureeos-testnet-cpu-stability-20261119-r1/AUTO_TRUST_PINS.json').read_text());actual={}
for n,expected in pins['contracts'].items():
 a=g.rpc('get_raw_abi',{'account_name':n},port=8890);actual[n]={k:a[k]for k in expected};assert actual[n]==expected
assert hashlib.sha256(Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos').read_bytes()).hexdigest()==g.PIN
accounts={}
for n in ['civicgovauth','civicgovlead','eosio.dev','civicbankers','eosio.token']:
 a=g.rpc('get_account',{'account_name':n},port=8890);accounts[n]={k:a[k]for k in ['account_name','privileged','permissions','ram_quota','ram_usage','cpu_limit','net_limit']}
s=g.rpc('get_upgrade_state',port=8890);assert not s['pending'] and s['review_seconds']==1800 and s['sealed']
result={'utc':g.now(),'info':info,'pins':actual,'tools':pins['tools'],'accounts':accounts,'governance':s,'stats':g.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'},port=8890),'bank':g.rpc('get_currency_balance',{'code':'eosio.token','account':'civicbankers','symbol':'AIC'},port=8890),'finality':g.rpc('get_finalizer_info',port=8902),'guard_inflight':g.load('GUARD_INFLIGHT.json'),'auto_latest':json.loads(Path('/opt/pureeos-testnet-cpu-stability-20261119-r1/AUTO_REPLACEMENT_LATEST.json').read_text())};print(json.dumps(result))
