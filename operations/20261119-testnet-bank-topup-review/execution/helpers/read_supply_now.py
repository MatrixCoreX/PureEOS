from pathlib import Path
import sys,json
sys.path.insert(0,'/opt/pureeos-testnet-stall-recovery-20261116-r1')
import stability_guard as g
s=g.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'},port=8890)
i=g.rpc('get_info',port=8890)
print(json.dumps({'utc':g.now(),'chain_id':i['chain_id'],'head_block_num':i['head_block_num'],'last_irreversible_block_num':i['last_irreversible_block_num'],'stats':s}))
