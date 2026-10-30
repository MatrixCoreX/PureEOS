from functional_30_common import *
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  assert topology['running_nodes']==30
  f.PLAN.write_text(f.PLAN.read_text()+'\n### M30-05角色检查纠正\n\n首轮对全部30实例一律要求已存在safety文件，误把9名从未在当前链投票的备用节点当作当前确认者。已保存首次FAIL和全部30进程审计：21名当前确认者均已有独立safety文件；9名备用未投票且未创建文件。定向复核严格检查当前/待生效确认者的持久状态，备用进入确认者角色时另查首次落盘；不预造空safety文件，也不清空或重写现有安全状态。\n')
  D=f.begin('M30-05','R04');preset=read(B/'input/chain-parameters.json')
  for n in nodes:
   cfg=f.rpc(n,'get_consensus_parameters')['chain_config'];assert all(cfg[k]==v for k,v in preset.items())
  profiles=[]
  for m in topology['machines']:profiles +=profile_host(m,[n for n in nodes if n['host_index']==m['host_index']])
  f.write(D/'all-thirty-processes-before-assert.json',profiles)
  policy=f.rpc(nodes[0],'get_finalizer_info');required={x['description']for x in policy['active_finalizer_policy']['finalizers']}
  if policy.get('pending_finalizer_policy'):required|={x['description']for x in policy['pending_finalizer_policy']['finalizers']}
  for r in profiles:
   n=next(n for n in nodes if n['account']==r['account']);assert r['uid']==n['uid']and r['threads']>=8
   if r['account']in required:assert r['safety'],'Current/pending finalizer missing persisted safety data: '+r['account']
  f.write(C/'ALL_THIRTY_PROCESS_PROFILES.json',profiles)
  f.finish('M30-05',dict(all_thirty_complete_CPU16_NET16_preset=True,mandatory_parallel_configuration_hashes_verified=True,actual_processes=profiles,current_and_pending_finalizers_with_safety=sorted(required),standby_without_prior_votes_may_have_no_file=True,source_commit=topology['source_commit'],P2P_payload_bytes=67108864,producer_queue_bytes=1073741824,shared_hardware_scope=topology['placement_scope']))
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
