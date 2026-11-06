"""Read-only outcome verification; no enrollment/payment actions may be repeated."""
from common import *
import traceback
try:
 assert not(R/'FINAL_VERIFY.json').exists()
 result=read('JOIN_RESULT.json');assert result['status']=='PASS'and read('RUN_PROGRESS.json')['status']=='PASS'
 plan=read('PLAN.json');base=read('BASELINE.json');start=rpc('get_info');assert start['chain_id']==CHAIN;original_pids()
 nodes=[];voters={v['owner']:v for v in table('votersv2')};producers={v['owner']:v for v in table('producersv2')};bonds={v['owner']:v for v in table('bpbonds')}
 proposer=rpc('get_producer_schedule');finalizer=rpc('get_finalizer_info');assert len(proposer['active']['producers'])==len(finalizer['active_finalizer_policy']['finalizers'])==21 and finalizer['active_finalizer_policy']['threshold']==15
 for node in plan['nodes']:
  process=state(node['unit']);assert process['ActiveState']=='active'and int(process['MainPID'])>0 and process['UnitFileState']=='enabled'
  info=rpc('get_info',port=node['rpc_port']);assert info['chain_id']==CHAIN and info['last_irreversible_block_num']>=start['last_irreversible_block_num']-24 and start['head_block_num']-info['head_block_num']<=24
  block=rpc('get_block',{'block_num_or_id':info['last_irreversible_block_num']});assert block['id']==info['last_irreversible_block_id']
  account=rpc('get_account',{'account_name':node['name']});auth={p['perm_name']:p['required_auth']for p in account['permissions']};assert not account['privileged']and auth['owner']==auth['active']==permission(node['public_keys']['account'])
  own=voters[node['name']];other=voters[node['voter']];assert own['producers']==other['producers']==[node['name']]
  assert int(producers[node['name']]['total_votes'])==int(own['staked'])+int(other['staked'])
  assert int(bonds[node['name']]['balance'])==100000000 and not int(bonds[node['name']]['forfeited'])
  nodes.append({'name':node['name'],'process':process,'info':info,'producer':producers[node['name']],'bond':bonds[node['name']],'self_vote':own,'other_vote':other,'current_proposer':node['name']in{p['producer_name']for p in proposer['active']['producers']},'current_finalizer':node['name']in{p['description']for p in finalizer['active_finalizer_policy']['finalizers']}})
 stops={u:state(u)for u in read('NODE_STOP_RESULT.json')['only_units']};assert all(v['ActiveState']=='inactive'and v['MainPID']=='0'and v['UnitFileState']=='disabled'for v in stops.values())
 old=Path('/opt/pureeos-bp-demo-six-20261105-r1');retired={u:state(u)for u in json.loads((old/'USER_THREE_STOP_RECEIPT_20261106.json').read_text())['only_five_units']};assert all(v['ActiveState']=='inactive'and v['MainPID']=='0'and v['UnitFileState']=='disabled'for v in retired.values())
 candidates=table('rotcands');assert {c['owner']for c in candidates if c['banned']}=={c['owner']for c in base['old_candidates']if c['banned']}and sum(bool(c['banned'])for c in candidates)==6
 maintenance=table('bpmaint');assert next(m for m in maintenance if m['owner']=='montrealmila')==next(m for m in base['maintenance']if m['owner']=='montrealmila')
 deadline=time.monotonic()+60;summary_reads=[]
 while True:
  with urllib.request.urlopen('http://127.0.0.1:9051/v2/state/get_producer_summary',timeout=15)as f:summary=json.load(f)
  summary_reads.append({'available':summary.get('available'),'registered':summary.get('registered_bp_count'),'eligible':summary.get('eligible_bp_count'),'head':summary.get('head')})
  if summary.get('available')and summary.get('chain_id')==CHAIN and summary.get('registered_bp_count')==32 and summary.get('eligible_bp_count')==25:break
  assert time.monotonic()<deadline,'Read-only summary has not caught up';time.sleep(2)
 assert summary['permanent_blacklist_count']==6 and summary['maintenance_active_count']==1 and summary['maintenance_pending_count']==0
 assert state(UNIT)['ActiveState']=='inactive';end=rpc('get_info');original_pids()
 assert end['last_irreversible_block_num']>start['last_irreversible_block_num']
 cpu=json.loads(subprocess.check_output(['lscpu','-J'],text=True));cpu={r['field'].rstrip(':'):r['data']for r in cpu['lscpu']};machine={k:cpu.get(k)for k in['Architecture','CPU(s)','Model name','Thread(s) per core','Core(s) per socket','Socket(s)','Hypervisor vendor']}
 save('FINAL_VERIFY.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'start':start,'end':end,'new_nodes':nodes,'only_two_user_stopped_services':stops,'prior_retired_five_still_off':retired,'preserved24_other_PIDs':base['node_pids'],'native_proposers':proposer,'native_finalizers':finalizer,'maintenance':maintenance,'producer_summary':summary,'summary_observations':summary_reads,'machine':machine,'memory':subprocess.check_output(['free','-m'],text=True),'loadavg':Path('/proc/loadavg').read_text().strip(),'token_stats_unchanged':base['token_stats'],'code_ABI_pins_unchanged':base['code_ABI_pins'],'scope':'New2synced/registered/self+other votes/own10000AICbonds, all32distinct vote totals, oldvotes/bonds/contracts/supplies/soleHTTP retained. Native21/15,32registered/25eligible/6banned/1maintenance. Baidu/Montreal servicesoff without deleting theirdata. Real newBP duty is not claimed; nativeFIFO applies. No server/reset/TPS/capacity qualification or wallet/device/private-key export.'})
 print('TWO_NEW_BP_AND_TWO_STOP_FINAL_VERIFICATION_PASS',flush=True)
except BaseException:
 save('FINAL_VERIFY_FAILURE.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
