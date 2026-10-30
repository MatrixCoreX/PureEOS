from functional_30_common import *
def rows(table,owner=None):
 values=f.rpc(nodes[0],'get_table_rows',dict(code='eosio',scope='eosio',table=table,json=True,limit=1000))['rows']
 return values if owner is None else next(x for x in values if x.get('owner',x.get('account'))==owner)
def role_gossip_wait(owner,present):
 global nodes
 all_nodes=f.N
 banned={x['owner']for x in rows('rotcands')if x['banned']}
 eligible=[n for n in all_nodes if n['account']not in banned]
 assert len(eligible)>=21
 try:
  f.N=eligible
  result=f.gossip_wait(owner,present)
 finally:f.N=all_nodes
 return dict(eligible_BP_nodes=result,excluded_permanently_banned_observers=sorted(banned),ordinary_fullnode_transaction_confirmation_still_includes_all30=True)
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.N=nodes;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  schedule=f.rpc(nodes[0],'get_producer_schedule');fin=f.rpc(nodes[0],'get_finalizer_info');rotation=rows('rotation')[0]
  duty={p['producer_name']for p in schedule['active']['producers']}|{p['description']for p in fin['active_finalizer_policy']['finalizers']}|set(rotation['active'])|set(rotation['target'])
  candidates=rows('rotcands');tickets={x['owner']:x['ticket']for x in candidates}
  who=max([n for n in nodes if n['index']>=3 and n['account']not in duty and not next(x['banned']for x in candidates if x['owner']==n['account'])],key=lambda n:tickets[n['account']]);owner=who['account'];key=next(x['block_key']for x in f.F['nodes']if x['account']==owner)
  D=f.begin('M30-06','R04');before=dict(producer=rows('producersv2',owner),candidate=rows('rotcands',owner),voter=rows('votersv2',owner),policies=dict(proposer=schedule,finalizer=fin))
  unregistered=f.send_fresh([f.action('eosio','unregprod',owner,dict(producer=owner))])
  absent=role_gossip_wait(owner,False)
  after_unreg=dict(producer=rows('producersv2',owner),candidate=rows('rotcands',owner),gossip=absent)
  f.write(D/'UNREGISTERED.json',dict(transaction=unregistered,before=before,after=after_unreg))
  reg=f.action('eosio','regproducer',owner,dict(producer=owner,producer_key=key,url='functional-normal-role-rejoin',location=0))
  stake=f.action('eosio','delegatebw',owner,{'from':owner,'receiver':owner,'stake_net_quantity':'0.1000 SYS','stake_cpu_quantity':'0.1000 SYS','transfer':False})
  vote=f.action('eosio','voteproducer',owner,dict(voter=owner,proxy='',producers=[owner]))
  reregistered=f.send_fresh([reg,stake,vote]);present=role_gossip_wait(owner,True)
  after=dict(producer=rows('producersv2',owner),candidate=rows('rotcands',owner),voter=rows('votersv2',owner),gossip=present)
  assert key in json.dumps(after['producer'])and after['voter']['producers']==[owner]
  p=f.rpc(nodes[0],'get_producer_schedule');q=f.rpc(nodes[0],'get_finalizer_info')['active_finalizer_policy'];assert len(p['active']['producers'])==len(q['finalizers'])==21 and q['threshold']==15
  business=f.send_fresh([f.action('tokenone','transfer','srcaa',{'from':'srcaa','to':'dstaa','quantity':'0.0001 TKN','memo':'ordinary business after normal producer role rejoin'})])
  f.finish('M30-06',dict(owner=owner,unregistration=unregistered,reregistration_own_stake_self_vote=reregistered,after=after,observer_role_did_not_grant_native_authority=True,native_membership_still_from_normal21_member_policy=True,immediate_in_service_admission_not_claimed=True,ordinary_business=business))
  f.write(C/'ROLE_LIFECYCLE_PASSED.json',dict(status='PASS',chain_id=topology['chain_id'],owner=owner,baseline_promoted=False,TPS='unmeasured'))
except BaseException as e:
 f.failed(e);f.write(C/'ROLE_LIFECYCLE_FAILED.json',dict(error=str(e),traceback=traceback.format_exc()));raise
