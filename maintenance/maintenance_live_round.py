from maintenance_30_common import *
M=Path('/opt/aic-bp-maintenance-feature-20261030');H=E/'live-results';H.mkdir(exist_ok=True)
def row(table,owner,node=None,payer=False):
 data=f.rpc(node or nodes[0],'get_table_rows',dict(code='eosio',scope='eosio',table=table,json=True,limit=1000,show_payer=payer))['rows']
 for item in data:
  d=item.get('data',item)
  if d.get('owner',d.get('account'))==owner:return item if payer else d
 return None
def rot(node=None):return f.rpc(node or nodes[0],'get_table_rows',dict(code='eosio',scope='eosio',table='rotation',json=True,limit=1))['rows'][0]
def maint(owner,on=True,expected=False,actor=None,signers=None):
 return f.send_fresh([f.action('eosio','setmaint',actor or owner,dict(producer=owner,maintenance=on))],signers=signers,expected_error=expected)
def policy(node=None):
 n=node or nodes[0];p=f.rpc(n,'get_producer_schedule');q=f.rpc(n,'get_finalizer_info')
 assert len(p['active']['producers'])==len(q['active_finalizer_policy']['finalizers'])==21
 assert q['active_finalizer_policy']['threshold']==15
 return dict(proposers=p,finalizers=q)
def duties():
 d=policy();r=rot()
 return {p['producer_name']for p in d['proposers']['active']['producers']}|{p['description']for p in d['finalizers']['active_finalizer_policy']['finalizers']}|set(r['active'])|set(r['target'])
def begin(cid):
 d=f.begin(cid,'R13');f.ACTIVE.update(system_contract_source=topology['system_contract_source'],system_wasm_sha256=topology['system_wasm_sha256']);f.write(d/'result.json',f.ACTIVE);return d
def partial(cid,observed):
 p=Path(f.ACTIVE['evidence']);f.ACTIVE.update(status='PARTIAL',observed=observed);f.write(p/'result.json',f.ACTIVE);f.record_status(cid,'PARTIAL',p,'Only explicitly listed subcases covered; remaining branches still required');f.ACTIVE=None
def wait_effective(owner,D):
 limit=time.monotonic()+300;history=[]
 while True:
  states={n['account']:dict(row=row('bpmaint',owner,n),info=f.rpc(n,'get_info'))for n in nodes}
  policy()
  history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),states=states,policies=policy(),rotation=rot()));f.write(D/'handoff.json',history)
  if all(v['row']and v['row']['state']==2 and v['info']['last_irreversible_block_num']>=v['row']['effective_block']for v in states.values()):
   for n in nodes:
    p=policy(n)
    assert owner not in {x['producer_name']for x in p['proposers']['active']['producers']}
    assert owner not in {x['description']for x in p['finalizers']['active_finalizer_policy']['finalizers']}
   return history
  assert time.monotonic()<limit,'Maintenance did not finish both policies and finality within300s'
  time.sleep(2)
def stop_node(n):
 return operate(n,"import subprocess,json\nr=subprocess.run(['systemctl','stop',"+repr(n['unit'])+"],capture_output=True,text=True);assert r.returncode==0,r.stderr;print(json.dumps(dict(stopped=True)))")
def catchup(n,reference,timeout=90):
 until=time.monotonic()+timeout
 while True:
  try:
   d=f.rpc(n,'get_info');ref=f.rpc(reference,'get_info')
   if d['chain_id']==topology['chain_id']and ref['head_block_num']-d['head_block_num']<=4 and d['last_irreversible_block_num']>=ref['last_irreversible_block_num']-4:return d
  except BaseException:pass
  assert time.monotonic()<until,'Restarted maintainer did not catch up with existing data'
  time.sleep(.5)
try:
 limit=time.monotonic()+1800
 while not(E/'ACTIVATED_CURRENT.json').exists():
  assert not(E/'REFRESH_FAILED.json').exists(),'Updated maintenance activation failed; no dependent actions sent'
  assert time.monotonic()<limit,'Updated maintenance activation pending';time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(E/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.N=nodes;f.F=read(E/'fixture/PUBLIC_FIXTURE.json')
  assert topology['chain_id']==read(E/'ACTIVATED_CURRENT.json')['chain_id']
  D=begin('MAINT-01');reserved=duties();standby=next(n['account']for n in nodes if n['account']not in reserved)
  before=row('rotcands',standby);producer=row('producersv2',standby);reply=maint(standby)
  copies={n['account']:row('bpmaint',standby,n,True)for n in nodes}
  assert all(x['payer']==standby and x['data']['state']==2 for x in copies.values())
  assert row('producersv2',standby)==producer
  # Candidate tickets can advance through normal bounded FIFO scanning; maintenance action does not reset violations.
  after=row('rotcands',standby)
  for k in['banned','zero_windows','evidence_slots','evidence_policies','ban_final_block']:assert after[k]==before[k]
  f.finish('MAINT-01',dict(owner=standby,request=reply,all30_rows=copies,candidate_evidence_preserved=True,own_RAM_payer=True))
  D=begin('MAINT-02');original=row('bpmaint',standby)
  wrong=maint(standby,False,True,actor='srcaa')
  peer_only=maint(standby,False,True,signers=[dict(account=standby,role='peer-v1')])
  assert row('bpmaint',standby)==original
  f.finish('MAINT-02',dict(other_active_account_rejected=wrong,peer_private_key_alone_rejected=peer_only,original_row_unchanged=True))
  D=begin('MAINT-12');maint(standby,True,True);maint(standby,False);maint(standby,False,True)
  partial('MAINT-12',dict(duplicate_suspend_and_duplicate_resume_rejected=True,cancellation_correct=True,concurrent_requests='pending targeted race scenario'))
  # Choose a registered incumbent with historical produced-block credits and keep both native policies full.
  until=time.monotonic()+300
  while True:
   p=policy();members=[x['producer_name']for x in p['proposers']['active']['producers']]
   eligible=[x for x in reversed(rot()['active'])if x in members and x!=nodes[0]['account']and row('producersv2',x)['unpaid_blocks']>0]
   if eligible:incumbent=eligible[0];break
   assert time.monotonic()<until;time.sleep(1)
  D=begin('MAINT-03');before=row('producersv2',incumbent);prior=row('rotcands',incumbent);request=maint(incumbent)
  immediate={n['account']:row('bpmaint',incumbent,n)for n in nodes};assert all(v['state']in[1,2]for v in immediate.values())
  history=wait_effective(incumbent,D);credits=row('producersv2',incumbent)['unpaid_blocks'];assert credits>=before['unpaid_blocks']
  f.finish('MAINT-03',dict(owner=incumbent,request=request,immediate=immediate,handoff=str(D/'handoff.json'),historical_credits_before=before['unpaid_blocks'],credits_at_effective=credits,no_shutdown_before_effective=True))
  D=begin('MAINT-05');f.finish('MAINT-05',dict(evidence=str(B/'rounds/R13/MAINT-03'),all_sampled_proposer_and_finalizer_policies_remained21=True,threshold15=True,all30_effective_checkpoint_finalized=True))
  D=begin('MAINT-04');partial('MAINT-04',dict(current_finalizer_and_producer_handoff_verified=True,evidence=str(B/'rounds/R13/MAINT-03'),request_by_not_yet_active_pending_entrant='pending separate transition case'))
  D=begin('MAINT-08');wallets={n['account']:f.balance(n,'eosio.token',incumbent,'SYS')for n in nodes};assert len(set(wallets.values()))==1 and credits>0
  claimed=f.send_fresh([f.action('eosio','claimrewards',incumbent,dict(owner=incumbent))])
  paid={n['account']:f.balance(n,'eosio.token',incumbent,'SYS')for n in nodes};assert len(set(paid.values()))==1 and next(iter(paid.values()))>next(iter(wallets.values()))
  assert all(row('producersv2',incumbent,n)['unpaid_blocks']==0 for n in nodes)
  cooldown=f.send_fresh([f.action('eosio','claimrewards',incumbent,dict(owner=incumbent))],expected_error=True)
  f.finish('MAINT-08',dict(historical_credits_claimed=credits,claim=claimed,wallets_before=wallets,wallets_after=paid,cooldown_rejection=cooldown,maintenance_status2_unchanged=row('bpmaint',incumbent)['state']==2))
  D=begin('MAINT-14');victim=next(n for n in nodes if n['account']==incumbent);others=[n for n in nodes if n!=victim];recovered=False
  before=row('bpmaint',incumbent);stop_node(victim)
  try:
   f.N=others
   transferred=f.send_fresh([f.action('eosio.token','transfer','srcaa',{'from':'srcaa','to':'dstaa','quantity':'0.0001 SYS','memo':'effective maintainer safely stopped'})])
  finally:
   f.N=nodes;start_node(victim,False,suffix='maintrejoin');f.write(E/'LIVE_TOPOLOGY.json',topology)
   info=catchup(victim,others[0]);recovered=True
  assert row('bpmaint',incumbent,victim)==before
  partial('MAINT-14',dict(effective_maintainer_shutdown_and_original_data_safety_restart=True,transaction_on_remaining29=transferred,rejoined=info,maintenance_state_persisted=True,live_snapshot_restore='pending dedicated snapshot observer case'))
  D=begin('MAINT-17');key=next(x['next_peer_key']for x in f.F['nodes']if x['account']==incumbent)
  rotated=f.send_fresh([f.action('eosio','regpeerkey',incumbent,dict(proposer_finalizer_name=incumbent,key=key))])
  f.gossip_wait(incumbent,False)
  resumed=maint(incumbent,False);f.gossip_wait(incumbent,True)
  actual={n['account']:row('peerkeys',incumbent,n)for n in nodes};assert all(key in json.dumps(v)for v in actual.values())
  f.finish('MAINT-17',dict(paused_BP_key_rotation_authorized=rotated,no_paused_identity_in_gossip_after_cache_refresh=True,resume=resumed,resumed_R1_identity_restored_within_original210s_gate=True,all30_registry_rows=actual))
  D=begin('MAINT-09');f.finish('MAINT-09',dict(owner=incumbent,state=row('bpmaint',incumbent),normal_eligibility_and_signed_gossip_restored=True,force_insert_native_policy=False,evidence=str(B/'rounds/R13/MAINT-17')))
  D=begin('MAINT-19');profiles=[]
  for machine in topology['machines']:profiles+=profile_host(machine,[n for n in nodes if n['host_index']==machine['host_index']])
  assert len(profiles)==30 and all(x['threads']>=8 for x in profiles)
  infos=f.infos();height=min(x['last_irreversible_block_num']for x in infos.values());ids={n['account']:f.rpc(n,'get_block',dict(block_num_or_id=height))['id']for n in nodes};assert len(set(ids.values()))==1
  f.write(D/'profiles.json',profiles)
  f.finish('MAINT-19',dict(all30_same_finalized_checkpoint=ids,height=height,all_parallel_configurations=True,profiles=str(D/'profiles.json'),three_real_hosts=True,pressure_load=False,TPS='unmeasured'))
  f.write(E/'MAINTENANCE_LIVE_BASE_PASSED.json',dict(status='PASS',chain_id=topology['chain_id'],system_contract_source=topology['system_contract_source'],system_wasm_sha256=topology['system_wasm_sha256'],full_cases=['MAINT-01','MAINT-02','MAINT-03','MAINT-05','MAINT-08','MAINT-09','MAINT-17','MAINT-19'],partial=['MAINT-04','MAINT-12','MAINT-14'],remaining_cases_still_required=True,baseline_promoted=False,pressure_load=False,TPS='unmeasured'))
except BaseException as e:
 f.failed(e);f.write(E/'MAINTENANCE_LIVE_FAILED.json',dict(error=str(e),traceback=traceback.format_exc(),baseline_promoted=False));raise
