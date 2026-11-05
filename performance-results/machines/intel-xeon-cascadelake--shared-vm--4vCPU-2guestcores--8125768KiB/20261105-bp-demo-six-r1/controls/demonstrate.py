"""Real offline-five bans and owner-authorized maintenance of the sixth."""
from common import *
import traceback
plan=read('PLAN.json');nodes={n['name']:n for n in plan['nodes']};targets=set(plan['fault_targets']);maint=plan['maintenance_target'];baseline=read('BASELINE.json');old={p['owner']for p in baseline['old_producers']}
status={'status':'WAITING_FOR_SIX_JOINS','started_UTC':now(),'targets':sorted(targets),'maintenance_target':maint,'chain_id':CHAIN};stopped=False
def report(phase,**data):status.update(status=phase,UTC=now(),**data);save('DEMO_PROGRESS.json',status)
def snapshot():
 i=rpc('get_info');assert i['chain_id']==CHAIN;original_pids()
 schedule=rpc('get_producer_schedule');fin=rpc('get_finalizer_info');cands=table('rotcands');bonds=table('bpbonds')
 unexpected=[r['owner']for r in cands if r['banned']and r['owner']not in targets];assert not unexpected,'Unexpected non-target native ban '+str(unexpected)
 return {'UTC':now(),'info':i,'schedule':schedule,'finalizers':fin,'candidates':cands,'bonds':bonds,'bond_config':table('bpbondcfg')[0],'maintenance':table('bpmaint'),'rotation':table('rotation')[0]}
def members(s):
 return {r['producer_name']for r in s['schedule']['active']['producers']},{r['description']for r in s['finalizers']['active_finalizer_policy']['finalizers']}
def fresh_window(s):
 # Inject the owned-node outage early in an actual admission round, before
 # any fault target's personal turn. This preserves the native window rules
 # and avoids removing a twice-missed seat just before its third proof.
 order=[x['producer_name']for x in s['schedule']['active']['producers']];span=len(order)*12
 slot=int(s['rotation']['last_slot']);first=min(order.index(n)*12 for n in targets)
 return s['rotation']['stage']==2 and int(s['rotation']['observation_start'])>slot and slot%span+24<first
def live_new(s):
 evidence=[]
 for name,n in nodes.items():
  u=state(n['unit']);assert u['ActiveState']=='active'and int(u['MainPID'])>0
  i=rpc('get_info',port=n['rpc_port']);assert i['chain_id']==CHAIN and s['info']['head_block_num']-i['head_block_num']<=24 and i['last_irreversible_block_num']>=s['info']['last_irreversible_block_num']-24
  evidence.append({'name':name,'process':u,'info':i})
 return evidence
def ensure_ban_final(row,s):
 assert row['banned']and int(row['zero_windows'])>=3 and 0<int(row['ban_final_block'])<=s['info']['last_irreversible_block_num']
 assert len(row['evidence_slots'])==len(row['evidence_policies'])==3 and len(set(row['evidence_slots']))==3 and all(int(x)>0 for x in row['evidence_slots'])
 bond=next(b for b in s['bonds']if b['owner']==row['owner']);assert bond['state']==4 and int(bond['balance'])==0 and int(bond['forfeited'])==100000000 and int(bond['ban_record_block'])<=s['info']['last_irreversible_block_num']
 b=rpc('get_block',{'block_num_or_id':int(row['ban_final_block'])})
 return {'owner':row['owner'],'native_candidate':row,'native_bond':bond,'ban_final_block':b['block_num'],'ban_final_block_id':b['id'],'observed_LIB':s['info']['last_irreversible_block_num']}
try:
 assert not(R/'CAMPAIGN_SUPERSEDED_20261106.json').exists(),'Superseded by explicit three-stop/five-dataset-retirement instruction; do not resume the old five-target controller'
 assert not(R/'DEMO_PROGRESS.json').exists(),'Reconcile this existing fault campaign before resuming'
 report('WAITING_FOR_SIX_JOINS');deadline=time.monotonic()+10800
 while not(R/'JOIN_RESULT.json').exists():
  if(R/'JOIN_PROGRESS.json').exists():assert read('JOIN_PROGRESS.json')['status']!='NEEDS_RECONCILIATION','Join requires reconciliation, no fault injected'
  assert time.monotonic()<deadline;time.sleep(5)
 assert read('JOIN_RESULT.json')['status']=='PASS'
 assert plan.get('maintenance_mode')=='immediate-owner-request'and read('MAINTENANCE_REQUEST.json')['status']=='PASS'
 report('WAITING_REAL_JOINT_DUTY_OF_FIVE_FAULT_TARGETS');deadline=time.monotonic()+21600
 while True:
  s=snapshot();p,f=members(s);proofs=read('DUTY_PROOFS.json')if(R/'DUTY_PROOFS.json').exists()else{};turns=proofs.get('complete_personal_production_turns',{})
  on=targets&p&f
  report('WAITING_REAL_JOINT_DUTY_OF_FIVE_FAULT_TARGETS',joint_fault_targets=sorted(on),completed_production_turns=turns,head=s['info']['head_block_num'],LIB=s['info']['last_irreversible_block_num'])
  if len(on)==5 and all(turns.get(n,0)>=1 for n in targets)and len(p)==len(f)==21 and p==f and s['finalizers']['active_finalizer_policy']['threshold']==15 and s['finalizers'].get('pending_finalizer_policy')is None and all(not r['zero_windows']for r in s['candidates']if r['owner']in targets)and fresh_window(s):
   assert not any(r['banned']for r in s['candidates'])
   votes={v['description']:v for v in s['finalizers'].get('last_tracked_votes',[])};remaining=f-targets
   strong=[name for name in remaining if name in votes and votes[name]['is_vote_strong']and votes[name]['voted_for_block_num']>=s['info']['last_irreversible_block_num']-12]
   if len(strong)==16 and s['info']['head_block_num']-s['info']['last_irreversible_block_num']<=64:
    health=live_new(s);again=snapshot();ap,af=members(again)
    if not targets<=ap&af or again['finalizers'].get('pending_finalizer_policy')is not None:time.sleep(2);continue
    if again['schedule']['active']['version']!=s['schedule']['active']['version']or again['finalizers']['active_finalizer_policy']['generation']!=s['finalizers']['active_finalizer_policy']['generation']:time.sleep(2);continue
    latest_votes={v['description']:v for v in again['finalizers'].get('last_tracked_votes',[])};remaining=af-targets
    strong=[name for name in remaining if name in latest_votes and latest_votes[name]['is_vote_strong']and latest_votes[name]['voted_for_block_num']>=again['info']['last_irreversible_block_num']-12]
    if len(strong)!=16 or any(r['zero_windows']for r in again['candidates']if r['owner']in targets)or not fresh_window(again):time.sleep(2);continue
    save('PRE_STOP.json',{'status':'PASS','snapshot':again,'new_node_sync':health,'production_proofs':proofs,'remaining_healthy_finalizers':sorted(remaining),'remaining_strong_votes':strong,'native_threshold':15,'scope':'All five have genuine irreversible production and current native proposer/finalizer duties. User superseded the sixth-node production prerequisite with immediate owner-authorized maintenance; no production-turn prerequisite applies to that node.'});break
  assert time.monotonic()<deadline,'Real joint duty gate has not occurred; no synthetic admission or altered timing'
  time.sleep(5)
 units=[nodes[n]['unit']for n in plan['fault_targets']];assert len(units)==5 and all(u.startswith('pureeos-bp-demo-')for u in units)
 report('STOPPING_ONLY_FIVE_AUTHORIZED_NEW_UNITS');started=now();subprocess.run(['systemctl','stop',*units],check=True,timeout=150);stopped=True
 subprocess.run(['systemctl','disable',*units],check=True,capture_output=True)
 assert all(state(u)['ActiveState']=='inactive'and state(u)['UnitFileState']=='disabled'for u in units)
 save('STOP_RECEIPT.json',{'status':'PASS','started_UTC':started,'finished_UTC':now(),'only_units':units,'original_PIDs':original_pids(),'maintenance_requested_for_faults':False,'keys_votes_bonds_registration_not_modified':True})
 report('FIVE_STOPPED_OBSERVING_THREE_NATIVE_ZERO_WINDOWS');deadline=time.monotonic()+21600;previous=rpc('get_info')['last_irreversible_block_num'];last_advance=time.monotonic();last_event=None;last_save=0
 while True:
  s=snapshot();lib=s['info']['last_irreversible_block_num']
  if lib>previous:previous=lib;last_advance=time.monotonic()
  if time.monotonic()-last_advance>90:
   candidates={r['owner']:r for r in s['candidates']};restore=[nodes[n]['unit']for n in targets if not candidates[n]['banned']]
   if restore:subprocess.run(['systemctl','start',*restore],check=True,timeout=150)
   save('FINALITY_SAFETY_RECOVERY.json',{'UTC':now(),'reason':'No irreversible progress90seconds; restore ONLY this campaign not-yet-banned nodes','restarted_only_unbanned_new_units':restore,'snapshot':s});raise RuntimeError('Fault demonstration interrupted for real finality recovery; do not claim completion')
  chosen=[r for r in s['candidates']if r['owner']in targets];banned=[r['owner']for r in chosen if r['banned']]
  event=tuple((r['owner'],r['zero_windows'],r['banned'])for r in chosen)
  if event!=last_event:save('events/ban-'+str(s['info']['head_block_num'])+'.json',s);last_event=event
  if time.monotonic()-last_save>30:
   with(R/'BAN_OBSERVATIONS.jsonl').open('a')as out:out.write(json.dumps(s)+'\n')
   last_save=time.monotonic()
  report('FIVE_STOPPED_OBSERVING_THREE_NATIVE_ZERO_WINDOWS',banned=sorted(banned),zero_windows={r['owner']:r['zero_windows']for r in chosen},head=s['info']['head_block_num'],LIB=lib)
  if len(banned)==5 and all(int(r['ban_final_block'])<=lib and int(next(b for b in s['bonds']if b['owner']==r['owner'])['ban_record_block'])<=lib for r in chosen):
   decisions=[ensure_ban_final(r,s)for r in chosen];save('FIVE_NATIVE_BANS_IRREVERSIBLE.json',{'status':'PASS','decisions':decisions,'snapshot':s});break
  assert time.monotonic()<deadline,'Native ban deadline; preserve real state without shortened windows';time.sleep(5)
 report('SETTLING_NATIVE_FORFEITURES_TO_LOCKED_POOL');settlements=[]
 with Wallet()as wallet:
  for name in plan['fault_targets']:
   s=snapshot();row=next(x for x in s['candidates']if x['owner']==name);decision=ensure_ban_final(row,s);bond=decision['native_bond']
   if int(bond['settled'])==0:
    proof=wallet.send('settle-'+name,[sysact('settlebond',BANK,{'producer':name})]);settlements.append({'name':name,'canonical_receipt':proof})
   else:assert int(bond['settled'])==100000000;settlements.append({'name':name,'already_settled_on_chain':True})
 s=snapshot();assert balance('eosio.bond')==2500000000 and balance('eosio.bslash')==500000000
 assert int(s['bond_config']['locked'])==2500000000 and int(s['bond_config']['slash_pending'])==0 and int(s['bond_config']['slash_settled'])==500000000
 for n in targets:
  b=next(x for x in s['bonds']if x['owner']==n);assert int(b['settled'])==100000000
 save('FORFEITURES.json',{'status':'PASS','settlements':settlements,'escrow_atoms':balance('eosio.bond'),'locked_pool_atoms':balance('eosio.bslash'),'bond_configuration':s['bond_config']})
 assert read('MAINTENANCE_REQUEST.json')['status']=='PASS'and state(nodes[maint]['unit'])['ActiveState']=='active'
 report('WAITING_NATIVE_MAINTENANCE_HANDOVER');deadline=time.monotonic()+10800
 while True:
  s=snapshot();p,f=members(s);m=next((x for x in s['maintenance']if x['owner']==maint),None)
  pending={x['description']for x in(s['finalizers'].get('pending_finalizer_policy')or{}).get('finalizers',[])}
  assert state(nodes[maint]['unit'])['ActiveState']=='active','Maintenance demonstration node stays online'
  report('WAITING_NATIVE_MAINTENANCE_HANDOVER',maintenance=m,head=s['info']['head_block_num'],LIB=s['info']['last_irreversible_block_num'])
  if m and m['state']==2 and 0<int(m['effective_block'])<=s['info']['last_irreversible_block_num']and maint not in p|f|pending:
   if not(R/'MAINTENANCE_EFFECTIVE.json').exists():save('MAINTENANCE_EFFECTIVE.json',{'status':'PASS','producer':maint,'native_state':m,'snapshot':s,'process_kept_online':state(nodes[maint]['unit'])})
   break
  assert time.monotonic()<deadline,'Maintenance native handover not yet effective; node must remain online';time.sleep(5)
 report('VERIFYING_FINAL_DEMO_STATE');s=snapshot();p,f=members(s);assert len(p)==len(f)==21 and s['finalizers']['active_finalizer_policy']['threshold']==15
 assert len(table('producersv2'))==30 and sum(bool(x['banned'])for x in s['candidates'])==5
 assert all(state(nodes[n]['unit'])['ActiveState']=='inactive'and state(nodes[n]['unit'])['UnitFileState']=='disabled'for n in targets)
 assert state(nodes[maint]['unit'])['ActiveState']=='active'
 voters={r['owner']:r for r in table('votersv2')};assert all(voters[x['owner']]==x for x in baseline['old_voters'])
 producers=table('producersv2');assert len({int(x['total_votes'])for x in producers})==30
 for n,sym in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]:assert rpc('get_currency_stats',{'code':n,'symbol':sym})==baseline['token_stats'][n]
 for n,pin in baseline['code_ABI_pins'].items():
  v=rpc('get_raw_abi',{'account_name':n});assert v['code_hash']==pin['code_hash']and v['abi_hash']==pin['abi_hash']
 for n in ['wallets','assets','feebuckets']:
  k={'wallets':'wallets','assets':'assets','feebuckets':'fees'}[n];assert table(n,'eosio.key','eosio.key')==baseline['KEY_snapshot'][k]
 assert all(table('balances','eosio.key',scope)==rows for scope,rows in baseline['KEY_snapshot']['balances'].items())
 save('NATIVE_DEMO_RESULT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'registered_BPs':30,'permanent_bans':5,'effective_maintenance':1,'eligible_BPs':24,'running_BP_processes':25,'history_processes':1,'actual_native_policy':'21/15','fault_targets':sorted(targets),'maintenance_target':maint,'original25_PIDs_unchanged':True,'all30_vote_totals_distinct':True,'old90_assignments_and_old_votes_unchanged':True,'escrow_atoms':balance('eosio.bond'),'locked_forfeiture_pool_atoms':balance('eosio.bslash'),'token_stats':baseline['token_stats'],'snapshot':s,'scope':'Actual native enrollment/production/offline bans/locked forfeiture/owner-authorized maintenance; no synthetic ban, shortened timing, quorum/contract/cap changes, reset, server or TPS.'})
 report('NATIVE_DEMO_PASS_AWAITING_PUBLIC_BROWSER')
except BaseException:
 report('NEEDS_RECONCILIATION',error=traceback.format_exc(),five_stop_performed=stopped);raise
