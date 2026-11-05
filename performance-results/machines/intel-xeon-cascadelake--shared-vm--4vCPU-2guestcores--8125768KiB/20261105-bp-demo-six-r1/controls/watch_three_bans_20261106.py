"""Read-only native blacklist follow-through after the user's three-node shutdown."""
from common import *
import traceback

stop=read('USER_THREE_STOP_RECEIPT_20261106.json');assert stop['status']=='PASS'
new=stop['selected_new_three'];old=stop['already_banned_two'];assert len(new)==3 and len(old)==2
deadline=time.monotonic()+43200;last=None
while time.monotonic()<deadline:
 try:
  info=rpc('get_info');assert info['chain_id']==CHAIN
  candidates={r['owner']:r for r in table('rotcands')};bonds={r['owner']:r for r in table('bpbonds')};rows=[]
  for name in old+new:
   c=candidates[name];b=bonds[name];rows.append({'producer':name,'banned':bool(c['banned']),'zero_windows':c['zero_windows'],'evidence_slots':c['evidence_slots'],'evidence_policies':c['evidence_policies'],'ban_final_block':c['ban_final_block'],'bond_record_block':b['ban_record_block'],'forfeited_atoms':b['forfeited'],'settled_atoms':b['settled'],'irreversible':bool(c['banned'])and 0<int(c['ban_final_block'])<=info['last_irreversible_block_num']and 0<int(b['ban_record_block'])<=info['last_irreversible_block_num']})
  pending=[r['producer']for r in rows if not r['irreversible']]
  progress={'status':'WAITING_NATIVE_THREE_NEW_BANS'if pending else'PASS','UTC':now(),'chain_id':CHAIN,'head':info['head_block_num'],'LIB':info['last_irreversible_block_num'],'all_five_retired_nodes':rows,'new_three_remaining':sum(r['producer']in new and not r['irreversible']for r in rows),'global_banned':[n for n,c in candidates.items()if c['banned']],'scope':'Read-only observation only, no stop/restart, data deletion, payment, settlement, key or native-state changes. Native windows and policy handovers govern timing.'}
  save('USER_THREE_NATIVE_BAN_PROGRESS_20261106.json',progress)
  event=[(r['producer'],r['zero_windows'],r['banned'])for r in rows]
  if event!=last:save('events/user-three-'+str(info['head_block_num'])+'.json',progress);last=event
  if not pending:
   progress['decisions']=[{'producer':r['producer'],'native_candidate':candidates[r['producer']],'native_bond':bonds[r['producer']],'anchor':{k:v for k,v in rpc('get_block',{'block_num_or_id':int(r['ban_final_block'])}).items()if k in['block_num','id','timestamp']}}for r in rows]
   save('USER_FIVE_NATIVE_BANS_RESULT_20261106.json',progress);print('FIVE_USER_ADOPTED_NATIVE_BANS_IRREVERSIBLE',flush=True);break
 except Exception:
  with(R/'USER_THREE_BAN_READ_ERRORS_20261106.jsonl').open('a')as out:out.write(json.dumps({'UTC':now(),'error':traceback.format_exc(),'scope':'Read-only failure; retry without changing native state'})+'\n')
 time.sleep(20)
else:
 save('USER_THREE_NATIVE_BAN_OBSERVATION_DEADLINE_20261106.json',{'status':'PENDING','UTC':now(),'scope':'Observation budget reached; no native timing or state bypass. Inspect current chain before any continuation.'})
