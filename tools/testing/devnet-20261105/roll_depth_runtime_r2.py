"""After qualification only: one current node at a time, same chain/data/keys/caps."""
from pathlib import Path
import datetime,hashlib,json,os,shutil,subprocess,time,urllib.request
C=Path('/opt/pureeos-dev-account-genesis-20261105-r1');R=Path('/opt/pureeos-devnet-20261105-r1');V=C/'runtime-5';old=C/'runtime-3'
assert not(R/'DEPTH_RUNTIME_UPGRADE.json').exists(),'Reconcile node-by-node upgrade before resuming'
q=json.loads((C/'qualification-9/RESULT.json').read_text());assert q['status']=='PASS'and sum(len(v['cases'])for v in q['suites'].values())==58
assert json.loads(Path('/opt/pureeos-key-atomic-ram-20261105-r1/attempt-15/qualification/RESULT.json').read_text())['status']=='PASS'
build=json.loads((C/'build-5/RESULT.json').read_text());assert build['status']=='BUILT_REQUIRES_QUALIFICATION';newhash=build['nodeos']['sha256'];assert build['source_commit']=='42dda604af169845b9ea9b9835fe0d25ecc126a8';assert newhash=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
def rpc(port,n,data={}):
 req=urllib.request.Request('http://127.0.0.1:'+str(port)+'/v1/chain/'+n,json.dumps(data).encode(),{'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=15)as r:return json.load(r)
info=rpc(8888,'get_info');chain=info['chain_id'];assert chain=='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
s=rpc(8888,'get_system_account_state');assert s['latest_proposal_id']==4 and s['pending_count']==0 and s['outcome']==2
assert json.loads((R/'NEW_CREATIONS_CANCELLATION.json').read_text())['status']=='FOUR_NEW_REVIEWS_CANCELLED_IRREVERSIBLE'
if not V.exists():
 subprocess.run(['cp','-a','--reflink=auto',str(old),str(V)],check=True);shutil.copy2(C/'build-5/nodeos',V/'libexec/nodeos')
assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==newhash
(V/'CANDIDATE_OVERLAY.json').write_text(json.dumps({'status':'DEV_TEST_RUNTIME_NOT_A_RELEASE','core_source_commit':build['source_commit'],'nodeos':build['nodeos'],'qualification':'Native58 at cap4 with actual fee WASM/fundedREX, atomicKEY238+SDK26','base_checksums':'Historical original package; this node overlay is authoritative','original_release_tags_unchanged':True},indent=2)+'\n')
for p in[V,*V.rglob('*')]:
 if not p.is_symlink()and p.is_dir():p.chmod(0o755)
plan=json.loads((R/'PLAN.json').read_text());entries=[{'name':'history','rpc_port':8890,'unit':'pureeos-devnet-history-20261105.service'}]+[{'name':e['name'],'rpc_port':e['rpc_port'],'unit':('pureeos-devnet-founder-20261105.service'if e['index']==0 else'pureeos-devnet-bp-'+e['name']+'-20261105.service')}for e in plan['BPs']]
# Founder last keeps the writer available throughout other one-node restarts.
entries.sort(key=lambda e:e['name']=='gptgenesisbp')
record={'status':'ROLLING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':chain,'old_nodeos_SHA256':plan['nodeos_SHA256'],'new_nodeos_SHA256':newhash,'source_commit':build['source_commit'],'qualified_cases':58,'completed':[],'parameters_before':rpc(8888,'get_consensus_parameters')}
def save():(R/'DEPTH_RUNTIME_UPGRADE.json').write_text(json.dumps(record,indent=2)+'\n')
save()
for e in entries:
 safety_deadline=time.monotonic()+90
 while True:
  before=rpc(8888,'get_info');assert before['chain_id']==chain
  if before['head_block_num']-before['last_irreversible_block_num']<30:break
  assert time.monotonic()<safety_deadline,'Wait for pre-reload finality guard';time.sleep(.5)
 fin=rpc(8888,'get_finalizer_info')['active_finalizer_policy'];assert len(fin['finalizers'])-1>=fin['threshold']
 unit=e['unit'];p=Path('/etc/systemd/system')/unit;text=p.read_text();oldpid=int(subprocess.check_output(['systemctl','show','-p','MainPID','--value',unit],text=True));assert oldpid>0
 archive=R/'archive'/('before-depth-runtime-'+unit);archive.write_text(text)
 if e['name']=='gptgenesisbp':
  wrapper=R/'tools-runtime/programs/nodeos/nodeos';(R/'archive/nodeos-wrapper-before-depth-runtime').write_bytes(wrapper.read_bytes());wrapper.write_text('#!/bin/sh\nexec '+str(V/'bin/nodeos')+' "$@"\n');wrapper.chmod(0o755)
 else:
  assert str(old)in text;p.write_text(text.replace(str(old),str(V)))
 subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','restart',unit],check=True)
 deadline=time.monotonic()+120
 while True:
  try:
   after=rpc(e['rpc_port'],'get_info')
   if after['chain_id']==chain and after['last_irreversible_block_num']>=before['last_irreversible_block_num']:break
  except Exception:pass
  assert time.monotonic()<deadline,'Reloaded current node did not synchronize';time.sleep(.4)
 newpid=int(subprocess.check_output(['systemctl','show','-p','MainPID','--value',unit],text=True));assert newpid!=oldpid and str(V/'libexec/nodeos').encode()in Path('/proc',str(newpid),'cmdline').read_bytes().split(b'\0')
 assert str(V/'libexec/nodeos')in Path('/proc',str(newpid),'maps').read_text()
 while rpc(8888,'get_info')['last_irreversible_block_num']<before['head_block_num']+2:assert time.monotonic()<deadline;time.sleep(.3)
 record['completed'].append({'name':e['name'],'unit':unit,'old_PID':oldpid,'new_PID':newpid,'synchronized_LIB':after['last_irreversible_block_num'],'version':after['server_full_version_string']});save();print('NODE_UPGRADED_AND_FINALITY_VERIFIED '+e['name'],flush=True)
assert len(record['completed'])==25
(R/'archive/PLAN.before-depth-runtime.json').write_bytes((R/'PLAN.json').read_bytes());plan['nodeos_SHA256']=newhash;plan['native_source_commit']=build['source_commit'];(R/'PLAN.json').write_text(json.dumps(plan,indent=2)+'\n')
p=C/'fresh_definitions.py';p.write_text(p.read_text().replace("V=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-3')","V=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5')"))
record.update(status='PASS',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),parameters_after=rpc(8888,'get_consensus_parameters'),info=rpc(8888,'get_info'),creation_state=rpc(8888,'get_system_account_state'),finalizers=rpc(8888,'get_finalizer_info'),scope='Same current chain/data/history/keys/stake/votes; bounded native creation dispatch repair, global depth4/caps unchanged, no new server/TPS/reset')
save();print('ALL25_CURRENT_NODE_DEPTH_RUNTIME_UPGRADE_PASS',flush=True)
