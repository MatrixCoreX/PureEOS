from maintenance_30_common import *
import shutil
M=Path('/opt/aic-bp-maintenance-feature-20261030');N=M/'native-tests'
H=E/'live-results';H.mkdir(exist_ok=True)
try:
 limit=time.monotonic()+1200
 while not(N/'fixture/MAINTENANCE_NATIVE_RESULT.json').exists():
  assert time.monotonic()<limit,'Updated contract native qualification did not finish'
  time.sleep(3)
 native=read(N/'fixture/MAINTENANCE_NATIVE_RESULT.json');assert native['status']=='PASS'
 build=read(M/'evidence/CONTRACT_BUILD_REPORT.json');assert build['source_commit']=='63255178dd824948afc723859cebe9321691a0e9'
 fresh=read(N/'fixture/PUBLIC_FIXTURE.json');assert fresh['system_code_hash']==build['system_wasm_sha256']
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(E/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];old_id=topology['chain_id'];assert fresh['chain_id']!=old_id
  old_head={n['account']:f.rpc(n,'get_info')for n in nodes}
  f.write(H/'OLD_CANDIDATE_FINAL_INFOS.json',old_head)
  # Stop only the separate maintenance chain, keeping the application chain alive.
  for machine in topology['machines']:
   members=[n for n in nodes if n['host_index']==machine['host_index']]
   root=E if machine['primary']else P
   code="from pathlib import Path\nimport subprocess,json,os,shutil\nroot=Path("+repr(str(root))+");out=[]\nfor n in "+repr(members)+":\n subprocess.run(['systemctl','stop',n['unit']],check=True,capture_output=True)\n d=root/'live'/n['account'];archive=d/('data-'+ "+repr(old_id[:12])+");assert not archive.exists();(d/'data').rename(archive);(d/'data').mkdir();os.chown(d/'data',n['uid'],n['gid']);(d/'data').chmod(0o700)\n shutil.copy2(d/'config/config.ini',d/('config-'+ "+repr(old_id[:12])+"+'.ini'))\n if(d/'node.log').exists():(d/'node.log').rename(d/('node-'+ "+repr(old_id[:12])+"+'.log'))\n out.append(dict(account=n['account'],old_unit=n['unit'],archived_data=str(archive)))\nprint(json.dumps(out))"
   f.write(H/('OLD_STATE_ARCHIVED-'+str(machine['host_index'])+'.json'),operate(machine,code))
  history=E/'history'/old_id[:12];history.mkdir(parents=True,exist_ok=False)
  shutil.copy2(E/'LIVE_TOPOLOGY.json',history/'LIVE_TOPOLOGY.json')
  (E/'fixture').rename(history/'fixture');(E/'fixture').mkdir()
  for filename in['PUBLIC_FIXTURE.json','PRIVATE_NODES.json','SIGNED_COMMANDS.json','bootstrap.snapshot','genesis.json']:
   p=E/'fixture'/filename;shutil.copy2(N/'fixture'/filename,p);p.chmod(0o600 if'PRIVATE'in filename else 0o644);os.chown(p,0,0)
  for machine in topology['machines'][1:]:
   for filename in['PUBLIC_FIXTURE.json','bootstrap.snapshot','genesis.json']:
    r=subprocess.run(['scp',*opts,str(E/'fixture'/filename),'root@'+machine['ip']+':'+str(P/'fixture'/filename)],capture_output=True,text=True,timeout=180);assert r.returncode==0,r.stderr[-1000:]
  previous='8758312bc222ba3ec5170383be60ea1daf42e5d423de90dafd7f9e95cb4e6c16'
  for n in nodes:
   root=E if n['primary']else P;path=root/'live'/n['account']/'config/config.ini'
   code="from pathlib import Path\nimport hashlib,json\np=Path("+repr(str(path))+");s=p.read_text();assert s.count("+repr(previous)+")==1;s=s.replace("+repr(previous)+","+repr(build['system_wasm_sha256'])+");p.write_text(s);print(json.dumps(dict(sha256=hashlib.sha256(s.encode()).hexdigest())))"
   n['configuration_sha256']=operate(n,code)['sha256']
  topology.update(chain_id=fresh['chain_id'],system_wasm_sha256=build['system_wasm_sha256'],system_contract_source=build['source_commit'],snapshot_sha256=hashlib.sha256((E/'fixture/bootstrap.snapshot').read_bytes()).hexdigest(),active_producer_accounts=fresh['active_producer_accounts'],active_finalizer_policy=fresh['active_finalizer_policy'],started=False,running_nodes=0)
  f.write(E/'LIVE_TOPOLOGY.json',topology);f.write(E/'INPUT_CONTRACT.json',build)
  f.T=topology;f.F=fresh;f.N=nodes
  launched=[]
  for n in nodes:
   launched.append(dict(account=n['account'],started=start_node(n,True,suffix='finauth')))
   topology['running_nodes']=len(launched);f.write(E/'LIVE_TOPOLOGY.json',topology);f.write(H/'CURRENT_LAUNCHED.json',launched)
  limit=time.monotonic()+300;history=[]
  while True:
   states=observations(nodes);good=all('info'in v and v['info']['chain_id']==fresh['chain_id']for v in states.values())
   if good:good=all(len({p['producer_name']for p in v['gossip']})==30 for v in states.values())and min(v['info']['last_irreversible_block_num']for v in states.values())>fresh['head_block_num']+2 and max(v['info']['head_block_num']for v in states.values())-min(v['info']['head_block_num']for v in states.values())<=4
   history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),nodes=states));f.write(H/'CURRENT_STARTUP_OBSERVATIONS.json',history)
   if good:break
   assert time.monotonic()<limit,'Updated full21/30-node maintenance chain failed original300s startup gate'
   time.sleep(3)
  expected=dict(read(E/'input/chain-parameters.json'),max_action_return_value_size=256)
  for n in nodes:
   actual=f.rpc(n,'get_consensus_parameters')['chain_config'];assert actual==expected,(n['account'],actual,expected)
   assert f.rpc(n,'get_code_hash',dict(account_name='eosio'))['code_hash']==build['system_wasm_sha256']
  topology.update(started=True,running_nodes=30);f.write(E/'LIVE_TOPOLOGY.json',topology)
  f.write(E/'ACTIVATED_CURRENT.json',dict(status='PASS',chain_id=fresh['chain_id'],system_contract_source=build['source_commit'],system_wasm_sha256=build['system_wasm_sha256'],node_source=topology['source_commit'],native_result=str(N/'fixture/MAINTENANCE_NATIVE_RESULT.json'),all30_signed_records=True,all_chain_parameters=expected,pressure_load=False,TPS='unmeasured',baseline_promoted=False))
except BaseException as e:
 f.write(E/'REFRESH_FAILED.json',dict(error=str(e),traceback=traceback.format_exc(),baseline_promoted=False));raise
