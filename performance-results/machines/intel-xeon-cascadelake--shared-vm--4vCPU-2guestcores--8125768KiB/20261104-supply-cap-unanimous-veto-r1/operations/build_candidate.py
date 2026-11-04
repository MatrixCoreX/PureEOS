"""Remote-only compile of the exact cap-veto payload; never deploys it."""
from pathlib import Path
import hashlib,json,shutil,subprocess,traceback
K=Path('/opt/pureeos-cap-veto-20261104-r1');S=K/'source';A=K/'artifacts';CDT=Path('/opt/pureeos-key-v1-20261103-r1/toolchain/pureeos-cdt-1.0.0')
def interface(abi):
 return {'types':{x['new_type_name']:x['type']for x in abi.get('types',[])},
  'structs':{x['name']:{'base':x['base'],'fields':x['fields']}for x in abi['structs']},
  'actions':{x['name']:x['type']for x in abi['actions']},
  'tables':{x['name']:{k:v for k,v in x.items()if k!='name'}for x in abi['tables']},
  'variants':{x['name']:x['types']for x in abi.get('variants',[])}}
try:
 assert not A.exists();(A/'generated').mkdir(parents=True)
 command=[str(CDT/'bin/cdt-cpp'),'-abigen','-contract=eosio.token','-O2','-I'+str(S/'include'),'-o',str(A/'generated/eosio.token.wasm'),str(S/'src/eosio.token.cpp')]
 with(K/'build.log').open('w')as log:subprocess.run(command,cwd=S,stdout=log,stderr=subprocess.STDOUT,check=True)
 assert interface(json.loads((A/'generated/eosio.token.abi').read_text()))==interface(json.loads((S/'abi/eosio.token.abi').read_text())),'Token ABI interface changed'
 shutil.copyfile(A/'generated/eosio.token.wasm',A/'eosio.token.wasm');shutil.copyfile(S/'abi/eosio.token.abi',A/'eosio.token.abi')
 files={str(p.relative_to(K)):hashlib.sha256(p.read_bytes()).hexdigest()for p in A.rglob('*')if p.is_file()}
 (K/'BUILD_RESULT.json').write_text(json.dumps({'status':'PASS','files':files,'ABI_interface_unchanged':True,'published_ABI_reused':True,'public_chain_changed':False,'command':command},indent=2)+'\n')
 print(json.dumps({'status':'PASS','WASM_sha256':files['artifacts/eosio.token.wasm']}))
except BaseException:
 (K/'BUILD_FAILURE.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
