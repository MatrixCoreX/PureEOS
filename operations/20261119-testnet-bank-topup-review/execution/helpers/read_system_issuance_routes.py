from pathlib import Path
import sys,json
G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');sys.path.insert(0,str(G));import stability_guard as g
abi=g.rpc('get_abi',{'account_name':'eosio'},port=8890)['abi'];structs={x['name']:x for x in abi['structs']};names=[a['name']for a in abi['actions']];related={a['name']:{'action':a,'struct':structs[a['type']]}for a in abi['actions']if any(x in a['name']for x in ['dev','gov','issue','fund','infl','pay','treas','claim'])};dev=g.rpc('get_account',{'account_name':'eosio.dev'},port=8890);print(json.dumps({'actions':names,'related':related,'dev_permissions':dev['permissions'],'upgrade_state':g.rpc('get_upgrade_state',port=8890)}))
