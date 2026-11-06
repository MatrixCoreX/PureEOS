"""One fresh native checkpoint for only these two new nodes."""
from common import *
import traceback

try:
 assert not(R/'SNAPSHOT_REQUEST.json').exists(),'Reconcile a prior checkpoint request; no blind repeat'
 i=rpc('get_info');assert i['chain_id']==CHAIN;original_pids()
 save('SNAPSHOT_REQUEST.json',{'status':'REQUESTING','UTC':now(),'chain_id':CHAIN,'before':i,'source':'Existing founder loopback native producer API, no service restart'})
 q=urllib.request.Request('http://127.0.0.1:8888/v1/producer/create_snapshot',b'{}',{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=300)as f:v=json.load(f)
 save('SNAPSHOT_NATIVE_RESPONSE.json',v);source=Path(v['snapshot_name']);assert source.is_file()and not source.is_symlink()
 target=R/'checkpoint/snapshot.bin';target.parent.mkdir(mode=0o755);target.parent.chmod(0o755);os.link(source,target)
 with target.open('rb')as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
 assert subprocess.run(['runuser','-u','pureeostest','--','test','-r',str(target)]).returncode==0
 deadline=time.monotonic()+300
 while rpc('get_info')['last_irreversible_block_num']<v['head_block_num']:assert time.monotonic()<deadline;time.sleep(1)
 block=rpc('get_block',{'block_num_or_id':v['head_block_num']});assert block['id']==v['head_block_id'];original_pids()
 save('SNAPSHOT_RESULT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'checkpoint_path':str(target),'checkpoint_SHA256':digest,'bytes':target.stat().st_size,'canonical_snapshot_block':{'block_num':block['block_num'],'block_id':block['id']},'native_response':v,'preserved24_other_PIDs':True,'scope':'Same-chain native irreversible trusted state checkpoint for two new identities; subsequent blocks fully validated. Raw checkpoint remains remote, no original node stopped or reconfigured.'})
 print('FRESH_NATIVE_CHECKPOINT_READY',block['block_num'],flush=True)
except BaseException:
 save('SNAPSHOT_FAILURE.json',{'status':'NEEDS_RECONCILIATION','UTC':now(),'error':traceback.format_exc()});raise
