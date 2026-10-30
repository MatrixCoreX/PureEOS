from maintenance_30_common import *
H=E/'live-results';H.mkdir(exist_ok=True)
try:
 assert read(E/'PREPARED.json')['status'].startswith('thirty-configurations')
 assert read(C0/'third-party-live/DEFI_BASE_PASSED.json')['all30_checked']
 assert read(C0/'worker-affinity-fix/QUALIFICATION.json')['status']=='PASS'
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(E/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.N=nodes;f.F=read(E/'fixture/PUBLIC_FIXTURE.json')
  assert topology['chain_id']!=read(C0/'LIVE_TOPOLOGY.json')['chain_id']
  # Only the owned peer source addresses can reach this separate functional chain.
  ips=[x['ip']for x in topology['machines']]
  for machine in topology['machines']:
   code="import subprocess,json\nresults=[]\nfor ip in "+repr(ips)+":\n r=subprocess.run(['ufw','allow','from',ip,'to','any','port','12000:12009','proto','tcp','comment','aic-bpmaint30-functional-20261030'],capture_output=True,text=True);assert r.returncode==0,r.stderr;results.append(dict(source=ip,range='12000:12009',returncode=r.returncode))\nprint(json.dumps(results))"
   f.write(H/('UFW-'+str(machine['host_index'])+'.json'),operate(machine,code))
  # Loopback RPC forwarding does not expose HTTP publicly.
  for machine in topology['machines'][1:]:
   args=['systemd-run','--unit=aic-bpmaint30-rpc-forward-'+str(machine['host_index'])+'-20261030','--property=Type=exec','--property=RuntimeMaxSec=86400','--property=RemainAfterExit=yes','/usr/bin/ssh',*opts,'-N','-o','ExitOnForwardFailure=yes','-o','ServerAliveInterval=15','-o','ServerAliveCountMax=4']
   for n in nodes:
    if n['host_index']==machine['host_index']:args+=['-L','127.0.0.1:'+str(n['http_port'])+':127.0.0.1:'+str(n['http_server_port'])]
   args+=['root@'+machine['ip']]
   r=subprocess.run(args,capture_output=True,text=True);assert r.returncode==0,r.stderr
  launched=[]
  for n in nodes:
   launched.append(dict(account=n['account'],started=start_node(n,True)))
   topology['running_nodes']=len(launched);f.write(E/'LIVE_TOPOLOGY.json',topology);f.write(H/'LAUNCHED.json',launched)
  deadline=time.monotonic()+300;history=[]
  while True:
   states=observations(nodes)
   good=all('info'in x and x['info']['chain_id']==topology['chain_id']for x in states.values())
   if good:
    good=all(len({p['producer_name']for p in x['gossip']})==30 for x in states.values())
    good=good and min(x['info']['last_irreversible_block_num']for x in states.values())>f.F['head_block_num']+2
    good=good and max(x['info']['head_block_num']for x in states.values())-min(x['info']['head_block_num']for x in states.values())<=4
   history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),nodes=states));f.write(H/'STARTUP_OBSERVATIONS.json',history)
   if good:break
   assert time.monotonic()<deadline,'Separate full21 maintenance chain did not qualify within original300s'
   time.sleep(3)
  build=read(Path('/opt/aic-bp-maintenance-feature-20261030/evidence/CONTRACT_BUILD_REPORT.json'))
  hashes={n['account']:f.rpc(n,'get_code_hash',dict(account_name='eosio'))['code_hash']for n in nodes}
  assert set(hashes.values())=={build['system_wasm_sha256']}
  preset=read(E/'input/chain-parameters.json')
  for n in nodes:
   assert f.rpc(n,'get_consensus_parameters')['chain_config']==preset
  policy=f.rpc(nodes[0],'get_finalizer_info')['active_finalizer_policy']
  assert len(f.rpc(nodes[0],'get_producer_schedule')['active']['producers'])==len(policy['finalizers'])==21 and policy['threshold']==15
  topology.update(started=True,running_nodes=30,maintenance_system_source=build['source_commit'],system_wasm_sha256=build['system_wasm_sha256'])
  f.write(E/'LIVE_TOPOLOGY.json',topology)
  f.write(E/'ACTIVATED.json',dict(status='PASS',chain_id=topology['chain_id'],code_hashes=hashes,full21_threshold15=True,all30_signed_records=True,original_application_chain_kept_running=True,pressure_load=False,TPS='unmeasured',baseline_promoted=False))
except BaseException as e:
 f.write(E/'ACTIVATION_FAILED.json',dict(error=str(e),traceback=traceback.format_exc(),baseline_promoted=False));raise
