from pathlib import Path
import os,json,subprocess,hashlib,time,traceback,fcntl,sys,concurrent.futures
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
B=Path('/opt/aic-bp-discovery-live-20261030');C=Path('/opt/aic-bp-discovery-30nodes-20261030');P=Path('/opt/aic-bp-discovery-30peer-20261030');J=Path('/opt/aic-bp-gossip-roster-resync-20261030')
sys.path.insert(0,str(B/'input'));import functional_common as f
def read(p):return json.loads(p.read_text())
def save(d):d['UTC']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime());f.write(C/'ACTIVATION_STATUS.json',d)
opts=['-i',str(B/'peer-access-key'),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(B/'peer_known_hosts')]
def operate(n,code):
 if n['primary']:r=subprocess.run(['python3','-B','-c',code],capture_output=True,text=True,timeout=180)
 else:r=subprocess.run(['ssh',*opts,'root@'+n['ip'],'python3 -B -'],input=code,capture_output=True,text=True,timeout=180)
 assert r.returncode==0,r.stderr[-1500:]
 return json.loads(r.stdout)if r.stdout.strip()else None
def start_node(n,initial=True,suffix=''):
 root=C if n['primary']else P;D=root/'live'/n['account'];unit=n['unit'] if not suffix else n['unit']+'-'+suffix
 args=['systemd-run','--unit='+unit,'--property=Type=exec','--property=User='+str(n['uid']),'--property=Group='+str(n['gid']),'--property=MemoryMax=5G','--property=MemorySwapMax=0','--property=CPUAffinity=0-15','--property=RuntimeMaxSec=86400','--property=RemainAfterExit=yes','--property=StandardOutput=append:'+str(D/'node.log'),'--property=StandardError=append:'+str(D/'node.log')]
 if n['primary']:
  args +=['--property=RootDirectory=/opt/aic-tps/rootfs','--property=BindReadOnlyPaths=/opt:/opt','--property=BindPaths='+str(C)+':/campaign',str(C/'runtime/nodeos'),'--config-dir','/campaign/live/'+n['account']+'/config','--data-dir','/campaign/live/'+n['account']+'/data']
  snapshot='/campaign/fixture/bootstrap.snapshot'
 else:
  args +=['--property=NoNewPrivileges=yes',str(P/'runtime/ld-linux-x86-64.so.2'),'--library-path',str(P/'runtime/lib'),str(P/'runtime/nodeos'),'--config-dir',str(D/'config'),'--data-dir',str(D/'data')];snapshot=str(P/'fixture/bootstrap.snapshot')
 if initial:args+=['--snapshot',snapshot]
 args+=['--chain-threads','6','--force-all-checks','--disable-replay-opts']
 code="import subprocess,os,json\nr=subprocess.run("+repr(args)+",capture_output=True,text=True);assert r.returncode==0,r.stderr\npid=int(subprocess.check_output(['systemctl','show',"+repr(unit)+",'-p','MainPID','--value'],text=True));assert pid>0;os.sched_setaffinity(pid,{"+str(n['local_index']%8)+"});print(json.dumps(dict(pid=pid,unit="+repr(unit)+",initial_snapshot="+str(initial)+")))"
 result=operate(n,code);n['unit']=unit;return result
def observations(nodes):
 result={}
 for n in nodes:
  try:
   info=f.rpc(n,'get_info');gossip=f.rpc(n,'bp_gossip_peers',ns='net');conns=f.rpc(n,'connections',ns='net')
   result[n['account']]=dict(info=info,gossip=gossip,connections=conns)
  except BaseException as e:result[n['account']]=dict(error=str(e)[:400])
 return result
def wait_stage(nodes,target,initial_head):
 limit=time.monotonic()+300;history=[]
 while True:
  states=observations(nodes);good=all('info'in x for x in states.values())
  if good:
   good=all(x['info']['chain_id']==f.T['chain_id']and len({p['producer_name']for p in x['gossip']})>=target for x in states.values())
   heads=[x['info']['head_block_num']for x in states.values()]
   good=good and max(heads)-min(heads)<=4
   if target>=21:good=good and min(x['info']['last_irreversible_block_num']for x in states.values())>initial_head+2
   else:assert max(x['info']['last_irreversible_block_num']for x in states.values())<=initial_head,'Sub-quorum group finalized newly produced blocks'
  history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),nodes=states));f.write(C/('stage-'+str(target)+'-observations.json'),history)
  if good:return states
  assert time.monotonic()<limit,'Stage '+str(target)+' discovery/finality did not reach the predeclared 300s gate'
  time.sleep(3)
def profile_host(machine,nodes):
 root=C if machine['primary']else P
 code="from pathlib import Path\nimport os,json,subprocess,hashlib,time\nroot=Path("+repr(str(root))+");out=[]\nfor n in "+repr(nodes)+":\n"
 code+=" D=root/'live'/n['account'];pid=int(subprocess.check_output(['systemctl','show',n['unit'],'-p','MainPID','--value'],text=True));assert pid>0\n status=(Path('/proc')/str(pid)/'status').read_text();m={line.split(':',1)[0]:line.split(':',1)[1].strip()for line in status.splitlines()if ':'in line}\n stat=(Path('/proc')/str(pid)/'stat').read_text().split(') ',1)[1].split();safety=[]\n for p in (D/'data').rglob('*'):\n  if p.is_file()and'safety'in p.name.lower():safety.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size))\n cfg=(D/'config/config.ini').read_text();assert hashlib.sha256(cfg.encode()).hexdigest()==n['configuration_sha256']\n out.append(dict(account=n['account'],pid=pid,uid=os.stat(Path('/proc')/str(pid)).st_uid,RSS_kB=int(m.get('VmRSS','0 kB').split()[0]),threads=int(m['Threads']),FDs=len(list((Path('/proc')/str(pid)/'fd').iterdir())),CPU_ticks=int(stat[11])+int(stat[12]),clock_ticks_per_second=os.sysconf('SC_CLK_TCK'),monotonic_ns=time.monotonic_ns(),safety=safety,configuration_sha256=hashlib.sha256(cfg.encode()).hexdigest(),startup=[line for line in (D/'node.log').read_text(errors='replace').splitlines()if('parallel'in line.lower()or'minimum'in line.lower()or'physical'in line.lower())and'PVT_'not in line][:12]))\nprint(json.dumps(out))"
 return operate(machine,code)
try:
 save(dict(status='waiting-for-three-machine-live-roster-regression-and-prepared-thirty-node-fixture',pressure_load=False,nodes_started=0))
 limit=time.monotonic()+2100
 while True:
  status=read(J/'STATUS.json')
  assert 'stopped-on-preserved-error'not in status['status'], 'Three-machine regression failed; do not activate larger topology'
  if status['status']=='three-node-roster-resynchronization-and-live-wire-regression-passed-next-plan-rounds-pending'and(C/'PREPARED.json').exists():break
  assert time.monotonic()<limit,'Prerequisite qualification did not finish'
  time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];fixture=read(C/'fixture/PUBLIC_FIXTURE.json')
  assert topology['source_commit']==read(J/'BUILD_REPORT.json')['source_commit']
  old=read(B/'LIVE_TOPOLOGY.json');assert old['source_commit']==topology['source_commit']
  save(dict(status='closing-three-machine-checkpoint-before-thirty-process-functional-simulation',pressure_load=False))
  # Retain the entire old chain and safety directories; align its final stopped state.
  f.write(C/'THREE_NODE_PRE_SWITCH.json',dict(topology=old,info={n['account']:f.rpc(n,'get_info')for n in old['nodes']}))
  for n in old['nodes']:f.rpc(n,'pause',ns='producer')
  until=time.monotonic()+30
  while True:
   heads={n['account']:f.rpc(n,'get_info')for n in old['nodes']}
   if len({v['head_block_id']for v in heads.values()})==1:break
   assert time.monotonic()<until,'Paused three-node heads did not align'
   time.sleep(1)
  integrity={n['account']:f.rpc(n,'get_integrity_hash',ns='producer')for n in old['nodes']}
  assert len({json.dumps(v,sort_keys=True)for v in integrity.values()})==1,'Three-node integrity checkpoints differ'
  f.write(C/'THREE_NODE_ALIGNED_CHECKPOINT.json',dict(info=heads,integrity=integrity,all_three_equal=True,data_and_safety_preserved_in_place=True))
  for n in old['nodes']:
   code="import subprocess,json\nsubprocess.run(['systemctl','stop',"+repr(n['unit'])+"],check=True);assert int(subprocess.check_output(['systemctl','show',"+repr(n['unit'])+",'-p','MainPID','--value'],text=True))==0\nprint(json.dumps(dict(stopped=True)))"
   operate(n,code)
  f.write(B/'THREE_NODE_STOPPED_FOR_30_SIMULATION.json',dict(status='three-node-chain-paused-checkpointed-and-gracefully-stopped; data-safety-retained',checkpoint=str(C/'THREE_NODE_ALIGNED_CHECKPOINT.json'),pressure_load=False))
  # One SSH process per peer carries its ten localhost-only RPC forwards.
  for m in topology['machines']:
   if m['primary']:continue
   args=['systemd-run','--unit=aic-bp30-rpc-forward-'+str(m['host_index'])+'-20261030','--property=RuntimeMaxSec=86400','--property=Restart=on-failure','--property=RestartSec=3','--property=StandardOutput=append:'+str(C/('forward-'+str(m['host_index'])+'.log')),'--property=StandardError=append:'+str(C/('forward-'+str(m['host_index'])+'.log')),'/usr/bin/ssh','-N',*opts,'-o','ExitOnForwardFailure=yes','-o','ServerAliveInterval=20']
   for n in nodes:
    if n['host_index']==m['host_index']:args+=['-L','127.0.0.1:'+str(n['http_port'])+':127.0.0.1:'+str(n['http_server_port'])]
   args+=['root@'+m['ip']]
   r=subprocess.run(args,capture_output=True,text=True);assert r.returncode==0,r.stderr
  f.T=topology;f.F=fixture;f.N=[]
  f.PLAN.write_text(f.PLAN.read_text()+'\nR04执行前细化：本扩展fixture已经按正常流程形成21名当前生产者及21名最终确认者，真实权重均1、门槛15。3/10实例阶段只验证发现、连接和追块，不宣称新块最终确认；可以完成snapshot中已认证旧块，不能在少于门槛时最终确认新产块。21/30阶段才要求新的全体确认进度。每批采用事先固定300秒门槛，不以失败后扩大门槛替代修复。\n')
  D=f.begin('M30-01','R04')
  assert len(nodes)==30 and len({(n['ip'],n['p2p_port'])for n in nodes})==30 and len({n['account']for n in nodes})==30 and len({n['uid']for n in nodes})==30
  f.finish('M30-01',dict(prepared=str(C/'PREPARED.json'),independent_accounts_keys_users_ports_data_and_safety_directories=True,source_commit=topology['source_commit'],parallel_workers_configured=8,started_yet=False))
  D=f.begin('M30-02','R04');stages={};launched=[]
  for target in [3,10,21,30]:
   save(dict(status='starting-functional-node-batch',target=target,already_running=len(launched),pressure_load=False))
   for n in nodes[len(launched):target]:
    start_node(n,True);launched.append(n);f.N=launched;topology['running_nodes']=len(launched);f.write(C/'LIVE_TOPOLOGY.json',topology)
   states=wait_stage(launched,target,fixture['head_block_num']);stages[str(target)]=dict(node_count=target,observed=states,live_finalizer_policy=f.rpc(launched[0],'get_finalizer_info'),quorum_phase='below quorum; no new-block finality claim'if target<15 else'new-block finality verified')
   f.write(D/'stages.json',stages)
  topology.update(started=True,running_nodes=30);f.write(C/'LIVE_TOPOLOGY.json',topology);f.T=topology;f.N=nodes
  f.finish('M30-02',dict(stages=str(D/'stages.json'),all_stages_reached_original_gates=True,no_saturation_or_TPS_workload=True))
  D=f.begin('M30-03','R04');final=stages['30']['observed']
  for n in nodes:
   own=final[n['account']];assert len({p['producer_name']for p in own['gossip']})==30
   assert n['seed_endpoint']in {ips for ips in [nodes[0]['ip']+':'+str(nodes[0]['p2p_port']),nodes[1]['ip']+':'+str(nodes[1]['p2p_port'])]}
  f.finish('M30-03',dict(one_static_seed_per_instance=True,all_thirty_signed_peer_records_learned=True,static_seed_configuration_hashes={n['account']:n['configuration_sha256']for n in nodes},observations=str(C/'stage-30-observations.json')))
  D=f.begin('M30-04','R04')
  desired={n['account']:n['ip']+':'+str(n['p2p_port'])for n in nodes}
  for n in nodes:
   peers=final[n['account']]['gossip']
   assert all(any(p['producer_name']==who and p['server_endpoint']==endpoint for p in peers)for who,endpoint in desired.items())
  f.finish('M30-04',dict(same_IP_distinct_ports_no_identity_aliasing=True,expected_endpoints=desired,three_host_cross_machine_gossip=True))
  D=f.begin('M30-05','R04');preset=read(B/'input/chain-parameters.json')
  for n in nodes:
   cfg=f.rpc(n,'get_consensus_parameters')['chain_config'];assert all(cfg[k]==v for k,v in preset.items())
  profiles=[]
  for m in topology['machines']:profiles +=profile_host(m,[n for n in nodes if n['host_index']==m['host_index']])
  for r in profiles:
   n=next(n for n in nodes if n['account']==r['account']);assert r['uid']==n['uid']and r['safety']and r['threads']>=8
  f.write(C/'ALL_THIRTY_PROCESS_PROFILES.json',profiles)
  f.finish('M30-05',dict(all_thirty_complete_CPU16_NET16_preset=True,all_parallel_mandatory_node_and_config_hashes_verified=True,actual_processes=profiles,source_commit=topology['source_commit'],P2P_payload_bytes=67108864,producer_queue_bytes=1073741824,shared_hardware_scope=topology['placement_scope']))
  D=f.begin('M30-07','R04');schedule=f.rpc(nodes[0],'get_producer_schedule');policy=f.rpc(nodes[0],'get_finalizer_info')
  active=schedule['active']['producers'];fp=policy['active_finalizer_policy']
  assert len(active)==len(fp['finalizers'])==21 and fp['threshold']==15
  assert all(p['producer_name']in desired for p in active)
  active_names={p['producer_name']for p in active};standby=sorted(set(desired)-active_names);assert len(standby)==9
  f.finish('M30-07',dict(active_producers=schedule,finalizer_policy=policy,nine_registered_standby_accounts=standby,actual_connections=str(C/'stage-30-observations.json')))
  D=f.begin('M30-09','R04');first=profiles;time.sleep(15);second=[]
  for m in topology['machines']:second +=profile_host(m,[n for n in nodes if n['host_index']==m['host_index']])
  measurements=[]
  for a in first:
   b=next(v for v in second if v['account']==a['account']);assert a['pid']==b['pid']
   seconds=(b['monotonic_ns']-a['monotonic_ns'])/1e9
   measurements.append(dict(account=a['account'],RSS_kB=b['RSS_kB'],threads=b['threads'],FDs=b['FDs'],average_logical_cores=(b['CPU_ticks']-a['CPU_ticks'])/a['clock_ticks_per_second']/seconds,sample_seconds=seconds))
  f.write(D/'measurements.json',measurements)
  f.finish('M30-09',dict(low_frequency_functional_resource_samples=measurements,TPS='unmeasured',thirty_processes_share_three_physical_machines=True))
  # Ordinary business transactions only after complete quorum and preparation.
  business=[]
  for i in range(6):
   code='eosio.token'if i%2==0 else'tokenone';symbol='SYS'if i%2==0 else'TKN';src=fixture['business_accounts'][2*i];dst=fixture['business_accounts'][2*i+1]
   before={n['account']:[f.balance(n,code,src,symbol),f.balance(n,code,dst,symbol)]for n in nodes}
   tx=f.send_fresh([f.action(code,'transfer',src,{'from':src,'to':dst,'quantity':'0.0001 '+symbol,'memo':'thirty-node-functional-'+str(i)+'-'+str(time.time_ns())})],node_index=i)
   after={n['account']:[f.balance(n,code,src,symbol),f.balance(n,code,dst,symbol)]for n in nodes}
   assert len({tuple(v)for v in before.values()})==len({tuple(v)for v in after.values()})==1
   assert all(after[k]==[v[0]-1,v[1]+1]for k,v in before.items())
   business.append(dict(transaction=tx,before=before,after=after,contract=code));f.write(C/'SIX_LOW_FREQUENCY_TRANSFERS.json',business)
  f.write(C/'ACTIVATED.json',dict(status='thirty-independent-parallel-node-functional-discovery-active',source_commit=topology['source_commit'],nodeos_sha256=topology['nodeos_sha256'],chain_id=topology['chain_id'],stages=[3,10,21,30],low_frequency_transfers=6,all_thirty_finalized_and_balances_match=True,pressure_load=False,TPS='unmeasured',full_suite_complete=False,baseline_promoted=False))
  save(dict(status='thirty-node-discovery-and-low-frequency-business-cases-passed-next-dropout-blacklist-and-third-party-rounds-pending',running_nodes=30,pressure_load=False,baseline_promoted=False))
except BaseException as e:
 f.failed(e);save(dict(status='thirty-node-activation-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),pressure_load=False,baseline_promoted=False));raise
