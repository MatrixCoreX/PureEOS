"""Read-only irreversible production-turn evidence for the new six BPs."""
from common import *
import traceback
plan=read('PLAN.json');names=set(plan['new_BPs']);cursor=plan.get('start_LIB',rpc('get_info')['last_irreversible_block_num'])+1
turns={n:{}for n in names};started=now();last_status=0
def slot(ts):
 d=datetime.datetime.fromisoformat(ts.rstrip('Z')).replace(tzinfo=datetime.timezone.utc)-datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)
 return(d.days*86400+d.seconds)*2+d.microseconds//500000
try:
 while not(R/'OBSERVER_FINISH').exists():
  writer=rpc('get_info');reader=rpc('get_info',port=8890);assert writer['chain_id']==reader['chain_id']==CHAIN
  end=min(writer['last_irreversible_block_num'],reader['last_irreversible_block_num']);last_slot=None
  for n in range(cursor,min(end,cursor+199)+1):
   b=rpc('get_block',{'block_num_or_id':n},port=8890);last_slot=slot(b['timestamp']);name=b['producer']
   if name in names:
    turn=str(last_slot//12);v=turns[name].setdefault(turn,{'turn':int(turn),'first_block_num':n,'last_block_num':n,'first_block_id':b['id'],'last_block_id':b['id'],'blocks':0,'slots':[]})
    v['last_block_num']=n;v['last_block_id']=b['id'];v['blocks']+=1;v['slots'].append(last_slot)
   cursor=n+1
  if time.monotonic()-last_status>=10:
   anchor=rpc('get_block',{'block_num_or_id':min(end,cursor-1)},port=8890);current_slot=slot(anchor['timestamp'])
   proofs={name:[v for _,v in sorted(items.items(),key=lambda x:int(x[0]))if(int(v['turn'])+1)*12<=current_slot]for name,items in turns.items()}
   save('DUTY_PROOFS.json',{'status':'OBSERVING','started_UTC':started,'UTC':now(),'chain_id':CHAIN,'scanned_through_block':cursor-1,'native_LIB':writer['last_irreversible_block_num'],'reader_LIB':reader['last_irreversible_block_num'],'complete_personal_production_turns':{n:len(v)for n,v in proofs.items()},'proofs':proofs,'scope':'Actual irreversible native block headers only. One turn is a distinct12-slot personal producer turn with at least one real block and finalized time past its end; actual produced block counts retained. No synthetic counters or inferred historical committees.'})
   last_status=time.monotonic()
  time.sleep(2)
except BaseException:
 save('HEADER_OBSERVER_FAILURE.json',{'status':'FAIL','UTC':now(),'cursor':cursor,'error':traceback.format_exc()});raise
