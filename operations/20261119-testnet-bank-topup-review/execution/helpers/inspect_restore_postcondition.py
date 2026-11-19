from pathlib import Path
import json,sys,traceback
R=Path('/opt/pureeos-testnet-bank-issuance-20261119-r1');sys.path.insert(0,str(R));import bank_upgrade as b
r={'utc':b.now(),'token':b.token_pin(),'stats':b.stats(),'bank_units':b.bank(),'governance':b.rpc('get_upgrade_state'),'scope':b.rpc('get_table_by_scope',{'code':'eosio.token','table':'testmint','limit':100}),'restore_progress':b.load('transactions/restore.json'),'signer_closed':b.load('SIGNER_restore_CLOSED.json')}
try:r['guard']=b.guard(b.OLD);r['guard_passed']=True
except Exception as e:r['guard_passed']=False;r['guard_error']=traceback.format_exc()
print(json.dumps(r))
