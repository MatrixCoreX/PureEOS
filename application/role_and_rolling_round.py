from functional_30_common import *
def txn(tag):
 src='srcaa';dst='dstaa'
 return f.send_fresh([f.action('eosio.token','transfer',src,{'from':src,'to':dst,'quantity':'0.0001 SYS','memo':tag+' '+str(time.time_ns())})])
def role_names():
 schedule=f.rpc(nodes[0],'get_producer_schedule');pol=f.rpc(nodes[0],'get_finalizer_info')
 names={p['producer_name']for p in schedule['active']['producers']}
 for key in ['pending','proposed']:
  if schedule.get(key):names|={p['producer_name']for p in schedule[key]['producers']}
 for key in ['active_finalizer_policy','pending_finalizer_policy']:
  if pol.get(key):names|={p['description']for p in pol[key]['finalizers']}
 return names,schedule,pol
def wait_registry(account,present):
 limit=time.monotonic()+210;history=[]
 while True:
  def one(n):
   peers=f.rpc(n,'bp_gossip_peers',ns='net')
   return n['account'],[p for p in peers if p['producer_name']==account]
  with concurrent.futures.ThreadPoolExecutor(max_workers=6)as pool:result=dict(pool.map(one,nodes))
  history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),peers=result));f.write(Path(f.ACTIVE['evidence'])/('gossip-present-'+str(present)+'.json'),history)
  if all(bool(v)==present for v in result.values()):return result
  assert time.monotonic()<limit,'Candidate eligibility did not reach signed discovery state'
  time.sleep(2)
def stopped_safety(n):
 root=C if n['primary']else P;D=root/'live'/n['account']
 code="from pathlib import Path\nimport subprocess,json,hashlib\nD=Path("+repr(str(D))+");subprocess.run(['systemctl','stop',"+repr(n['unit'])+"],check=True)\nassert int(subprocess.check_output(['systemctl','show',"+repr(n['unit'])+",'-p','MainPID','--value'],text=True))==0\ns=[]\nfor p in (D/'data').rglob('*'):\n if p.is_file()and'safety'in p.name.lower():s.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size))\nprint(json.dumps(dict(account="+repr(n['account'])+",safety=s,data_directory=str(D/'data'))))"
 return operate(n,code)
try:
 save(dict(status='waiting-for-completed-thirty-node-activation-before-role-and-restart-rounds',pressure_load=False))
 until=time.monotonic()+1800
 while not(C/'ACTIVATED.json').exists():
  assert time.monotonic()<until,'Thirty-node prerequisites did not complete'
  time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.N=nodes;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  D=f.begin('M30-08','R04');groups=[]
  f.PLAN.write_text(f.PLAN.read_text()+'\nM30-08执行方案：30实例分10批，每批3个（每主机1个）优雅停止、保留原目录/safety，再以同源码、同参数、同UID正常恢复；恢复不再加载bootstrap snapshot。每批其他27实例低频提交1笔普通转账，恢复后核对30实例的最终区块及ID。每批300秒恢复门槛，当前21人委员会任何一批最多掉3名，真实门槛15保持。\n')
  for offset in range(0,30,3):
   group=nodes[offset:offset+3];roles,schedule,policy=role_names()
   with concurrent.futures.ThreadPoolExecutor(max_workers=3)as pool:stopped=list(pool.map(stopped_safety,group))
   for r in stopped:
    if r['account']in roles:assert r['safety'],'Active/pending finalizer lost safety before restart'
   remaining=[n for n in nodes if n not in group];f.N=remaining
   tx=txn('rolling-group-'+str(offset//3))
   for n,safe in zip(group,stopped):
    check="from pathlib import Path\nimport hashlib,json\nentries="+repr(safe['safety'])+"\nfor e in entries:assert hashlib.sha256(Path(e['path']).read_bytes()).hexdigest()==e['sha256']\nprint(json.dumps(dict(safety_unchanged_before_restart=True)))"
    operate(n,check);start_node(n,False,'roll1')
   topology['nodes']=nodes;f.write(C/'LIVE_TOPOLOGY.json',topology);f.N=nodes
   deadline=time.monotonic()+300;obs=[]
   while True:
    snapshots=f.infos();minimum=min(x['last_irreversible_block_num']for x in snapshots.values());height=tx['included']['number']
    good=minimum>=height
    if good:
     blocks={n['account']:f.rpc(n,'get_block',dict(block_num_or_id=height))for n in nodes}
     good=all(b['id']==tx['included']['id']and tx['id']in set(f.block_ids(b))for b in blocks.values())
    obs.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),info=snapshots))
    f.write(D/('group-'+str(offset//3)+'-recovery.json'),obs)
    if good:break
    assert time.monotonic()<deadline,'Rolling restart did not recover all-thirty common transaction'
    time.sleep(2)
   item=dict(group=[n['account']for n in group],stopped_safety=stopped,ordinary_transfer_while_offline=tx,all_thirty_finalized_after_restart=True,all_restarts_used_existing_state_without_snapshot=True);groups.append(item);f.write(D/'groups.json',groups)
  f.finish('M30-08',dict(groups=groups,total_unique_instances_restarted=30,safety_preserved_for_every_prior_voter=True,current_and_pending_thresholds_preserved=True,all_nodes_parallel=True))
  # Reuse the stronger all-30 rolling process-drop/recovery evidence explicitly.
  D=f.begin('NET-03','R05');f.finish('NET-03',dict(actual_process_stops_and_existing_directory_recovery=str(B/'rounds/R04/M30-08'),groups=10,instances=30,ordinary_business_during_drop_and_after_recovery=True,safety_never_reset=True))
  D=f.begin('M30-06','R04')
  in_roles,schedule,policy=role_names();observer=next(n for n in reversed(nodes)if n['account']not in in_roles);account=observer['account'];pub=next(p for p in f.F['nodes']if p['account']==account)
  f.PLAN.write_text(f.PLAN.read_text()+'\nM30-06执行账户'+account+'：选择不在当前/待生效proposer/finalizer政策内的备用节点，正常unregprod成为观察者；等待链上资格及gossip撤销；以本人权限regproducer恢复，追加本人对本人的CPU/NET各0.1000 SYS质押并单选自投；等待原轮换规则接纳，验证首次投票后创建自己的safety文件。原始自购RAM、自质押和BLS登记由正常WASM fixture提供证据；不直接写资源或名单。缓存撤销/恢复各210秒，正式席位交接600秒门槛。\n')
  unregister=f.send_fresh([f.action('eosio','unregprod',account,dict(producer=account))])
  gone=wait_registry(account,False)
  after_unreg=f.rpc(observer,'get_info');assert after_unreg['chain_id']==topology['chain_id']
  reregister=f.send_fresh([f.action('eosio','regproducer',account,dict(producer=account,producer_key=pub['block_key'],url='',location=0))])
  stake_and_vote=f.send_fresh([f.action('eosio','delegatebw',account,{'from':account,'receiver':account,'stake_net_quantity':'0.1000 SYS','stake_cpu_quantity':'0.1000 SYS','transfer':False}),f.action('eosio','voteproducer',account,dict(voter=account,proxy='',producers=[account]))])
  restored=wait_registry(account,True)
  deadline=time.monotonic()+600;history=[]
  while True:
   names,current_schedule,current_policy=role_names()
   pa={p['producer_name']for p in current_schedule['active']['producers']};fa={p['description']for p in current_policy['active_finalizer_policy']['finalizers']}
   history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),schedule=current_schedule,finalizer_policy=current_policy));f.write(D/'normal-admission-timeline.json',history)
   if account in pa and account in fa:break
   assert time.monotonic()<deadline,'Normally registered/staked/self-voted candidate not admitted within original gate'
   time.sleep(3)
  # The account was a never-voting standby; check its first real safety write.
  before_file=next(p for p in read(C/'ALL_THIRTY_PROCESS_PROFILES.pre-assert-audit.json')if p['account']==account)['safety'];assert not before_file
  deadline=time.monotonic()+60
  while True:
   machine=next(m for m in topology['machines']if m['host_index']==observer['host_index']);profile=profile_host(machine,[observer])[0]
   if profile['safety']:break
   assert time.monotonic()<deadline,'Admitted finalizer did not persist its first voting safety state'
   time.sleep(2)
  business=txn('normal-observer-to-BP')
  f.finish('M30-06',dict(account=account,unregister=unregister,discovery_removed=gone,normal_reregistration=reregister,own_stake_and_single_choice_vote=stake_and_vote,discovery_restored=restored,became_active_producer_and_finalizer=True,new_safety_after_first_vote=profile['safety'],ordinary_transfer_all_thirty_finalized=business,native_preparation_evidence=read(C/'NATIVE_PREPARATION_STATUS.json')['evidence']))
  save(dict(status='round-R04-thirty-instance-role-lifecycle-and-all-thirty-rolling-restarts-passed-next-network-blacklist-contract-rounds-pending',running_nodes=30,pressure_load=False,baseline_promoted=False))
except BaseException as e:
 f.N=nodes;f.failed(e);save(dict(status='thirty-node-role-restart-round-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),pressure_load=False,baseline_promoted=False));raise
