"""Export one native same-chain checkpoint; never pause/reset an original node."""
from common import *
import shutil,traceback
assert not(R/'SNAPSHOT_REQUEST.json').exists(),'Reconcile an existing native snapshot request instead of recreating it'
before=rpc('get_info');assert before['chain_id']==CHAIN;original_pids()
save('SNAPSHOT_REQUEST.json',{'status':'REQUESTING_NATIVE_SNAPSHOT','UTC':now(),'chain_id':CHAIN,'before':before,'source':'existing founder producer API on loopback8888','original_nodes_paused_or_restarted':False})
try:
 q=urllib.request.Request('http://127.0.0.1:8888/v1/producer/create_snapshot',b'{}',{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=300)as f:result=json.load(f)
 save('SNAPSHOT_NATIVE_RESPONSE.json',result)
 source=Path(result['snapshot_name']);assert source.is_file()and not source.is_symlink()
 snapshot=R/'checkpoint/snapshot.bin';snapshot.parent.mkdir(mode=0o755,exist_ok=True);assert not snapshot.exists()
 os.link(source,snapshot)
 with snapshot.open('rb')as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
 deadline=time.monotonic()+300
 while True:
  info=rpc('get_info');assert info['chain_id']==CHAIN
  if info['last_irreversible_block_num']>=result['head_block_num']:break
  assert time.monotonic()<deadline,'Retain native checkpoint until irreversible';time.sleep(1)
 block=rpc('get_block',{'block_num_or_id':result['head_block_num']});assert block['id']==result['head_block_id']
 original_pids()
 save('SNAPSHOT_RESULT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'source_node':'gptgenesisbp','native_response':result,'checkpoint_path':str(snapshot),'checkpoint_SHA256':digest,'bytes':snapshot.stat().st_size,'canonical_snapshot_block':{'block_num':block['block_num'],'block_id':block['id'],'observed_LIB':info['last_irreversible_block_num']},'original25_PIDs_unchanged':True,'scope':'Native exported same-chain trusted state checkpoint for ONLY six new demo BPs; all later blocks use full validation. Original chain/history untouched; no unsafe live-state directory copy or consensus/signature bypass.'})
 print('NATIVE_SAME_CHAIN_IRREVERSIBLE_CHECKPOINT_READY',result['head_block_num'],snapshot.stat().st_size,flush=True)
except BaseException:
 save('SNAPSHOT_FAILURE.json',{'status':'NEEDS_NATIVE_SNAPSHOT_RECONCILIATION','UTC':now(),'error':traceback.format_exc()});raise
