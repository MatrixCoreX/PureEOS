from maintenance_30_common import *
def stop(n):
 return operate(n,"import subprocess,json\nsubprocess.run(['systemctl','stop',"+repr(n['unit'])+"],check=True,capture_output=True);print(json.dumps(dict(stopped=True)))")
def slot(info):
 import datetime
 return int((datetime.datetime.fromisoformat(info['head_block_time']).replace(tzinfo=datetime.timezone.utc).timestamp()-946684800)*2)
def stable_policy():
 until=time.monotonic()+600
 while True:
  r=f.rpc(nodes[0],'get_table_rows',dict(code='eosio',scope='eosio',table='rotation',json=True,limit=1))['rows'][0]
  info=f.rpc(nodes[0],'get_info');q=f.rpc(nodes[0],'get_finalizer_info')
  if r['stage']==2 and r['observation_end']-slot(info)>120 and not q.get('pending_finalizer_policy'):
   p=q['active_finalizer_policy'];assert len(p['finalizers'])==21 and p['threshold']==15
   assert all(x['weight']==1 for x in p['finalizers']);return p
  assert time.monotonic()<until,'No long stable21-member observation window for exact quorum test'
  time.sleep(2)
def info_all(group):return {n['account']:f.rpc(n,'get_info')for n in group}
def restore(victims):
 out=[]
 for n in victims:
  out.append(dict(account=n['account'],restart=start_node(n,False,suffix='quorumret')))
  f.write(E/'LIVE_TOPOLOGY.json',topology)
 return out
def wait_rejoin(group,ref):
 until=time.monotonic()+120
 while True:
  try:
   infos=info_all(group);reference=f.rpc(ref,'get_info')
   if all(x['chain_id']==topology['chain_id']and reference['head_block_num']-x['head_block_num']<=4 and x['last_irreversible_block_num']>=reference['last_irreversible_block_num']-4 for x in infos.values()):return infos
  except BaseException:pass
  assert time.monotonic()<until,'Quorum-test nodes did not rejoin'
  time.sleep(1)
try:
 limit=time.monotonic()+1800
 while not(E/'MAINTENANCE_LIVE_BASE_PASSED.json').exists():
  assert time.monotonic()<limit;time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(E/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.N=nodes;f.T=topology;f.F=read(E/'fixture/PUBLIC_FIXTURE.json')
  D=f.begin('CON-02','R06');steps=[]
  for online_count in[15,14]:
   policy=stable_policy();active={x['description']for x in policy['finalizers']}
   eligible=[n for n in nodes if n['account']in active and n['index']!=0]
   victims=eligible[:21-online_count];remaining=[n for n in nodes if n not in victims];stopped=[]
   try:
    for n in victims:stop(n);stopped.append(n)
    f.N=remaining;initial=info_all(remaining);cutoff=max(x['head_block_num']for x in initial.values())
    if online_count==15:
     transfer=f.send_fresh([f.action('eosio.token','transfer','srcab',{'from':'srcab','to':'dstab','quantity':'0.0001 SYS','memo':'exact quorum15 of21'})])
     current=f.rpc(remaining[0],'get_finalizer_info')['active_finalizer_policy']
     assert current==policy,'Policy changed during exact quorum15 measurement'
     observed=dict(online_finalizer_weight=15,required=15,new_transaction_finalized=transfer)
    else:
     time.sleep(12);after=info_all(remaining);current=f.rpc(remaining[0],'get_finalizer_info')['active_finalizer_policy']
     assert current==policy and all(x['last_irreversible_block_num']<=cutoff for x in after.values())
     observed=dict(online_finalizer_weight=14,required=15,no_new_post_cutoff_block_finalized=True,start=initial,cutoff_head=cutoff,after=after)
   finally:
    f.N=nodes;restarts=restore(stopped)
   recovered=wait_rejoin(stopped,remaining[0]);observed.update(restored_all=True,restarts=restarts,recovered=recovered);steps.append(observed);f.write(D/'steps.json',steps)
  final=f.send_fresh([f.action('tokenone','transfer','srcaa',{'from':'srcaa','to':'dstaa','quantity':'0.0001 TKN','memo':'all30 restored after exact quorum cases'})])
  f.finish('CON-02',dict(steps=steps,ordinary_business_after_recovery=final,committee_size_always21=True,threshold_unchanged15=True,safety_files_preserved=True,pressure_load=False))
  f.write(E/'EXACT_QUORUM_PASSED.json',dict(status='PASS',chain_id=topology['chain_id'],source_commit=topology['source_commit'],pressure_load=False,TPS='unmeasured'))
except BaseException as e:
 f.N=nodes;f.failed(e);f.write(E/'EXACT_QUORUM_FAILED.json',dict(error=str(e),traceback=traceback.format_exc()));raise
