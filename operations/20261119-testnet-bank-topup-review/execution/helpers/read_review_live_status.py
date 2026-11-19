from pathlib import Path
import sys,json
sys.path.insert(0,'/opt/pureeos-testnet-stall-recovery-20261116-r1');import stability_guard as g
r={'utc':g.now(),'info':g.rpc('get_info',port=8890),'stats':g.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'},port=8890),'bank':g.rpc('get_currency_balance',{'code':'eosio.token','account':'civicbankers','symbol':'AIC'},port=8890),'upgrade_state':g.rpc('get_upgrade_state',port=8890),'contracts':{}}
for n in ['eosio','eosio.token']:
 p=g.rpc('get_raw_abi',{'account_name':n},port=8890);r['contracts'][n]={k:p[k]for k in ['code_hash','abi_hash']}
p=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1/AUTO_REPLACEMENT_LATEST.json');a=json.loads(p.read_text());r['auto_replacement']={k:v for k,v in a.items() if k in ['status','utc','bank_units','required_bank_units','CPU_pressure_avg60','eligible']};print(json.dumps(r))
