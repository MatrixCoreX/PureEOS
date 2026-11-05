"""Final old-chain archive: freeze reads, stop only the recorded original26 nodes; retain all data."""
from pathlib import Path
import datetime,hashlib,json,subprocess,time,urllib.request
R=Path('/opt/pureeos-devnet-20261105-r1');C=Path('/opt/pureeos-dev-account-genesis-20261105-r1');O=Path('/opt/pureeos-keynet-20261103-r2');P=R/'archive'
assert not(P/'OLD_FINAL.json').exists(),'Never replay cutover'
pre=json.loads((C/'PREFLIGHT.json').read_text());frozen=json.loads((C/'OLD_ARCHIVE_PREPARE.json').read_text())
def rpc(n,d=None):
 q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+n,json.dumps(d or{}).encode(),{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=20)as r:return json.load(r)
assert rpc('get_info')['chain_id']==frozen['old_chain_id']
assert all((Path('/proc')/str(pid)).exists()for pid in pre['node_pids'].values())
assert json.loads((O/'activity/STATE.json').read_text()).get('pending')is None
# Four admission/signing services were never resumed; all original read apps are refrozen deliberately.
units=list(frozen['services'])
subprocess.run(['systemctl','stop',*units],check=True,timeout=180)
for x in frozen['containers']:
 row=json.loads(subprocess.check_output(['docker','inspect',x['id']],text=True))[0];assert row['Id']==x['id']and row['Image']==x['image']
 if row['State']['Running']:subprocess.run(['docker','stop','--time','40',x['id']],check=True,timeout=60,stdout=subprocess.DEVNULL)
info=rpc('get_info');deadline=time.monotonic()+120
while rpc('get_info')['last_irreversible_block_num']<info['head_block_num']:
 assert time.monotonic()<deadline;time.sleep(.5)
final={'status':'ARCHIVING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'info':rpc('get_info'),'governance':rpc('get_upgrade_state'),'consensus':rpc('get_consensus_parameters'),'finalizers':rpc('get_finalizer_info'),'ordinary':json.loads((O/'activity/STATE.json').read_text()),'original_nodes':pre['node_pids'],'old_apps':units,'containers':frozen['containers'],'old_root':str(O),'files':{}}
assert final['governance']['effective_veto_count']==24 and final['governance']['proposal_id']==3
for unit in [*units,*pre['node_pids']]:
 p=Path('/etc/systemd/system')/unit
 if p.exists():(P/unit).write_bytes(p.read_bytes());final['files'][unit]=hashlib.sha256(p.read_bytes()).hexdigest()
(P/'OLD_FINAL_PRESTOP.json').write_text(json.dumps(final,indent=2)+'\n')
subprocess.run(['systemctl','disable',*units,*pre['node_pids']],check=True,capture_output=True)
subprocess.run(['systemctl','stop',*pre['node_pids']],check=True,timeout=180)
assert all(not(Path('/proc')/str(pid)).exists()for pid in pre['node_pids'].values())
assert(O/'founder/data/blocks').exists()and(O/'history/state-history').exists()
final.update(status='OLD_ARCHIVED_ALL26_STOPPED',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),all_raw_data_retained=True,no_new_chain_ID_reuse=True)
(P/'OLD_FINAL.json').write_text(json.dumps(final,indent=2)+'\n');print('OLD_CHAIN_ARCHIVED_NOT_DELETED',flush=True)
