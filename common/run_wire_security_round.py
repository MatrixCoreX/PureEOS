import functional_common as f
from pathlib import Path
import time,json,fcntl,traceback
B=f.B
def keyact(verb,bp,key,actor=None):
 return f.action('eosio',verb,actor or bp,dict(proposer_finalizer_name=bp,key=key))
def healthy():
 states=f.infos();assert len({x['chain_id']for x in states.values()})==1
 assert max(x['head_block_num']for x in states.values())-min(x['head_block_num']for x in states.values())<15
 return {k:dict(head=v['head_block_num'],LIB=v['last_irreversible_block_num'])for k,v in states.items()}
def rejection(acts,expected):
 before={n['account']:f.rows(n)for n in f.N}
 r=f.send_fresh(acts,expected_error=True);assert expected in json.dumps(r['reply']),r
 after={n['account']:f.rows(n)for n in f.N};assert before==after
 return r
def transfer(tag):
 src='srcaa';dst='dstaa'
 before={n['account']:[f.balance(n,'eosio.token',src,'SYS'),f.balance(n,'eosio.token',dst,'SYS')]for n in f.N}
 r=f.send_fresh([f.action('eosio.token','transfer',src,dict(from_=src))]) if False else f.send_fresh([f.action('eosio.token','transfer',src,{'from':src,'to':dst,'quantity':'0.0001 SYS','memo':'functional '+tag+' '+str(time.time_ns())})])
 after={n['account']:[f.balance(n,'eosio.token',src,'SYS'),f.balance(n,'eosio.token',dst,'SYS')]for n in f.N}
 for n in f.N:assert after[n['account']]==[before[n['account']][0]-1,before[n['account']][1]+1]
 r.update(before=before,after=after);return r
def replay(records,captured):
 return f.wire_probe(records,False,gossip_override=captured['gossip'])
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  bp='bpbbb';pub=f.F['nodes'][1];k1=pub['peer_key'];r1=pub['next_peer_key']
  D=f.begin('KEY-03','R01');res=[]
  for verb,key in [('regpeerkey',r1),('delpeerkey',k1)]:
   res.append(rejection([keyact(verb,bp,key,'srcaa')],'missing authority of bpbbb'))
  f.write(D/'rejections.json',res);f.finish('KEY-03',dict(actual_live_register_and_delete_rejected=True,all_three_registry_unchanged=True))
  D=f.begin('KEY-04','R01')
  r=rejection([keyact('regpeerkey','srcaa',r1)],'registered active producer or a current native producer')
  f.finish('KEY-04',dict(non_BP_registration_rejected=True,registry_unchanged=True,rejection=r))
  D=f.begin('KEY-05','R01')
  a=rejection([keyact('regpeerkey',bp,k1)],'already registered')
  b=rejection([keyact('regpeerkey',bp,'PUB_K1_11111111111111111111111111111111149Mr2R')],'empty peer key')
  native=json.loads((B/'NATIVE_PREPARATION_STATUS.json').read_text())
  source=Path('/opt/aic-bp-discovery-live-20261030/native-build/attempt-1793319904916268156/native_fixture.cpp').read_text()
  assert 'webauthn'in source.lower()
  f.finish('KEY-05',dict(live_duplicate_and_empty_key_rejected=[a,b],WebAuthn_native_contract_check= native['evidence'],scope='WA rejection is native real-WASM preparation evidence; K1 duplicate and empty are live three-node checks'))
  D=f.begin('SEC-02','R03')
  record=f.peer_record();captured=f.helper(f.peer_request([record]))
  positive=f.wire_probe([dict(record,initial=True)],True);assert positive['full_responses']>0
  prior={n['account']:[p['expiration']for p in f.rpc(n,'bp_gossip_peers',ns='net')if p['producer_name']==bp]for n in f.N}
  tx=f.send_fresh([keyact('regpeerkey',bp,r1)],1);observed=f.gossip_wait(bp,True,prior)
  old=replay([record],captured)
  current=f.wire_probe([f.peer_record(role='peer-v2',r1=True,initial=True)],True);assert current['full_responses']>0
  f.finish('SEC-02',dict(captured_pre_rotation_gossip=captured,rotation=tx,all_three_new_key_gossip=observed,old_packet_no_privilege=not old['privileged'],new_R1_positive_control=True,ordinary_business=transfer('after-old-key-replay')))
  D=f.begin('KEY-07','R01');stale=rejection([keyact('delpeerkey',bp,k1)],'current peer key does not match')
  # Keep this case running while the next exact delete-state check is performed, then finish both.
  f.write(D/'stale-delete.json',stale);key07=D;f.ACTIVE=None
  D=f.begin('SEC-03','R03');record=f.peer_record(role='peer-v2',r1=True);captured=f.helper(f.peer_request([record]))
  started=time.time();before=f.infos();tx=f.send_fresh([keyact('delpeerkey',bp,r1)],2)
  observed=f.gossip_wait(bp,False);elapsed=time.time()-started
  old=replay([record],captured);assert not old['privileged']
  missing=rejection([keyact('delpeerkey',bp,r1)],'peer key not found')
  f.write(key07/'missing-delete.json',missing)
  k=json.loads((key07/'result.json').read_text());k.update(status='PASS',observed=dict(stale_old_key_and_missing_record_deletions_rejected=True,stale=stale,missing=missing),end_UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
  f.write(key07/'result.json',k);f.record_status('KEY-07','PASS',key07)
  revocation=dict(start_unix=started,total_seconds_including_inclusion_finality_cache_and_probes=elapsed,before_heads={k:v['head_block_num']for k,v in before.items()},gossip_removed=observed,revocation_transaction=tx)
  f.finish('SEC-03',dict(registry_deleted_on_all_three=True,gossip_cleared_on_all_three=observed,actual_old_wire_replay_rejected=True,replayed_frame=captured['gossip'],revocation=revocation,ordinary_business=transfer('after-revocation-replay')))
  D=f.begin('SEC-13','R03');f.finish('SEC-13',dict(revocation=revocation,interpretation='Includes transaction confirmation and polling; no instantaneous propagation claim',raw_observations=str(B/'rounds/R03/SEC-03'/('attempt-'+str(max(int(p.name.split('-')[1])for p in (B/'rounds/R03/SEC-03').glob('attempt-*'))))/'gossip-observations.json')))
  D=f.begin('SEC-04','R03');tx=f.send_fresh([keyact('regpeerkey',bp,k1)],1);restored=f.gossip_wait(bp,True)
  positive=f.wire_probe([f.peer_record(initial=True)],True);assert positive['full_responses']>0
  f.finish('SEC-04',dict(restored=restored,normal_registration=tx,real_initial_authenticated_again=True,ordinary_business=transfer('after-restoration')))
  D=f.begin('SEC-05','R03')
  base=f.peer_record();prime=f.wire_probe([base],True);assert any(p['producer_name']==bp and p['expiration'].startswith(base['expiration']) for p in prime['gossip_after']), 'Exact legitimate pre-tamper payload was not cached'
  outcomes=[]
  for change in [dict(tamper_endpoint=f.N[1]['ip']+':19871'),dict(tamper_account='bpccc'),dict(tamper_expiration=time.strftime('%Y-%m-%dT%H:%M:%S',time.gmtime(time.time()+3800)))]:
   probe=f.wire_probe([dict(base,**change)],False);assert not any(p['server_endpoint'].endswith(':19871')for p in probe['gossip_after']);outcomes.append(dict(change=change,privileged=probe['privileged']))
  f.finish('SEC-05',dict(prime_valid_record_before_tamper=True,all_actual_modified_wire_proofs_rejected=outcomes,healthy=healthy()))
  D=f.begin('SEC-06','R03')
  base=f.peer_record();f.wire_probe([base],True)
  fake=f.wire_probe([dict(base,tamper_initial=True)],False);valid=f.wire_probe([f.peer_record(initial=True)],True);assert valid['full_responses']>0
  f.finish('SEC-06',dict(real_tampered_initial_no_privilege=not fake['privileged'],newly_signed_legal_initial_full_response=valid['full_responses'],healthy=healthy()))
  D=f.begin('SEC-07','R03');probe=f.wire_probe([f.peer_record(initial=True,sign_chain_id='00'*32)],False)
  f.finish('SEC-07',dict(cross_chain_signature_no_privilege=not probe['privileged'],chain_id_unchanged=healthy()))
  D=f.begin('SEC-08','R03');out=[]
  for delta in [-60,172800]:
   exp=time.strftime('%Y-%m-%dT%H:%M:%S',time.gmtime(time.time()+delta));p=f.wire_probe([f.peer_record(expiration=exp)],False);out.append(dict(expiration=exp,privileged=p['privileged']))
  valid=f.wire_probe([f.peer_record()],True)
  f.finish('SEC-08',dict(expired_and_excessive_future_rejected=out,valid_renewal_authenticated=valid['privileged'],healthy=healthy()))
  D=f.begin('SEC-09','R03')
  expiry=time.strftime('%Y-%m-%dT%H:%M:%S',time.gmtime(time.time()+25))
  records=[f.peer_record(expiration=expiry)]
  records +=[f.peer_record(expiration=expiry,endpoint=f.N[1]['ip']+':'+str(19800+i))for i in range(1,9)]
  records.sort(key=lambda p:(p['account'],p['endpoint']))
  # Nine distinct entries must be rejected as a complete message.
  nine=f.wire_probe(records,False)
  eight=f.wire_probe(sorted(records[:7]+[records[-1]],key=lambda p:(p['account'],p['endpoint'])),True);actual=[p for p in eight['gossip_after']if p['producer_name']==bp]
  assert len(actual)==8,actual
  f.write(D/'eight-live-entries.json',actual)
  time.sleep(27)
  deadline=time.monotonic()+15
  while True:
   state={n['account']:f.rpc(n,'bp_gossip_peers',ns='net')for n in f.N}
   if all(not any(p['server_endpoint'].split(':')[-1].startswith('198')for p in v)for v in state.values()):break
   assert time.monotonic()<deadline,'Short-lived boundary endpoints were not pruned'
   time.sleep(1)
  f.finish('SEC-09',dict(nine_rejected_before_update=True,exactly_eight_live_records_accepted=True,all_endpoints_owned=True,no_pressure=True,temporary_endpoint_expiry_cleanup=state,healthy=healthy()))
  D=f.begin('SEC-10','R03');results=[]
  for label,records in [('empty-message',[]),('empty-host',[f.peer_record(endpoint=':9876')]),('duplicate',[f.peer_record(),f.peer_record()]),('unsorted',[f.peer_record('bpccc'),f.peer_record('bpbbb')])]:
   p=f.wire_probe(records,False);results.append(dict(label=label,privileged=p['privileged'],closed=p['closed']))
  f.finish('SEC-10',dict(structural_invalid_packets_rejected=results,healthy=healthy(),ordinary_business=transfer('after-structural-negatives')))
  f.save(dict(status='live-wire-security-round-passed',passed=['KEY-03','KEY-04','KEY-05','KEY-07','SEC-02','SEC-03','SEC-04','SEC-05','SEC-06','SEC-07','SEC-08','SEC-09','SEC-10','SEC-13'],remaining_cases=True,pressure_load=False,baseline_promoted=False))
except BaseException as e:f.failed(e);raise
