from pathlib import Path
import sys,json,hashlib,subprocess,time
G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');R=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1');sys.path.insert(0,str(G));import stability_guard as g
assert not(R/'FOUNDER_READER_RESTORE_STARTED.json').exists();nodes=g.load('GUARD_NODE_MAP.json');e=g.load('GUARD_INFLIGHT.json');assert e['target']=='gptgenesisbp';v=nodes['gptgenesisbp'];reader=g.rpc('get_info',port=8890);assert reader['chain_id']==g.CHAIN and g.age(reader['head_block_time'])<6 and g.age(reader['last_irreversible_block_time'])<20
rows={};cursor=0
for _ in range(64):
 q={'json':True,'code':'eosio','scope':'eosio','table':'rotcands','limit':20,'time_limit_ms':1000,'key_type':'i64','index_position':'primary'}
 if cursor:q['lower_bound']=str(cursor)
 p=g.rpc('get_table_rows',q,port=8890)
 for x in p['rows']:rows[x['owner']]=x
 if not p['more']:break
 assert p['rows'];cursor=g.owner_primary_key(p['rows'][-1]['owner'])+1
else:raise AssertionError('Incomplete independent reader ban list')
record={'utc':g.now(),'founder_row':rows['gptgenesisbp'],'unit_before':g.state(v['unit']),'event':e['event']};print(json.dumps(record),flush=True)
assert not rows['gptgenesisbp']['banned'],'Permanently banned founder: never resurrect; reviewed shared-role handoff required'
assert g.sha(g.NATIVE)==g.PIN;cfg=Path(v['config_dir'])/'config.ini';sf=Path(v['data_dir'])/'finalizers/safety.dat';assert g.sha(cfg)==v['config_sha256'];assert sf.is_file()and g.sha(sf)==e['safety_sha256_before_start'];assert g.sha(G/e['event']/'safety.after-clean-stop.dat')==g.sha(sf);assert g.rpc('get_block',{'block_num_or_id':e['anchor_num']},port=8890)['id']==e['anchor_id'];assert all(g.state(nodes[n]['unit'])['MainPID']==pid for n,pid in e['other_PIDs'].items());(R/'FOUNDER_READER_RESTORE_STARTED.json').write_text(json.dumps(record,indent=2)+'\n')
if record['unit_before']['MainPID']=='0':subprocess.run(['systemctl','start',v['unit']],capture_output=True,check=True,timeout=20)
end=time.monotonic()+90;proofs=[]
while time.monotonic()<end:
 try:
  okay,proof=g.positive_reconciliation(e,nodes)
  if okay:proofs.append(proof)
  else:proofs=[]
  if len(proofs)>=3:break
 except Exception:proofs=[]
 time.sleep(2)
assert len(proofs)>=3,'Retain same data and inspect restored founder; no second stop'
subprocess.run(['systemctl','stop',e['watchdog']+'.timer'],capture_output=True,timeout=10,check=False);e.update(status='PASS_RECONCILED_SAME_OPERATION',phase='VERIFIED',utc=g.now(),proof=proofs[-1]);g.save(e['event']+'/RECONCILED.json',e);g.save('GUARD_INFLIGHT.json',e);g.save('GUARD_LAST.json',e)
progress=json.loads((R/'VOTE_ONE_ROLL_PROGRESS.json').read_text());assert len(progress['completed'])==22;assert 'gptgenesisbp'not in progress['completed'];progress['completed'].append('gptgenesisbp');progress['operations'].append({'target':'gptgenesisbp','operation':e,'note':'Safe reader-verified start after stopping the primary RPC source; no second stop, safety bytes preserved'});progress.update(status='PASS_ALL23_ONE_VOTE_THREAD_SAME_DATA',utc=g.now());(R/'VOTE_ONE_ROLL_RESULT.json').write_text(json.dumps(progress,indent=2)+'\n');record.update(status='PASS_FOUNDER_READER_VERIFIED_SAME_DATA_RESTORE',utc=g.now(),proofs=proofs);(R/'FOUNDER_READER_RESTORE_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
