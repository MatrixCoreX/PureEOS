"""Read-only proof of twelve real finalized zero-production windows for the new four bans."""
from finance_common import *
NEW=['alibabacloud','deepmindlabs','openaiclouds','sydneyunswai']
assert not(R/'CANONICAL_NEW_BAN_AUDIT.json').exists()
def slot(s):
 d=datetime.datetime.fromisoformat(s.rstrip('Z')).replace(tzinfo=datetime.timezone.utc)-datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)
 return(d.days*86400+d.seconds)*2+d.microseconds//500000
def stamp(s):return(datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)+datetime.timedelta(seconds=s/2)).isoformat()
i=rpc('get_info',port=8890);assert i['chain_id']==CHAIN
rows={r['owner']:r for r in table('rotcands')};bonds={r['owner']:r for r in table('bpbonds')};cache={}
def block(n):
 if n not in cache:cache[n]=rpc('get_block',{'block_num_or_id':n},port=8890)
 return cache[n]
result=[]
for name in NEW:
 r=rows[name];assert r['banned']and r['zero_windows']==3 and int(r['ban_final_block'])<=i['last_irreversible_block_num']
 windows=[]
 for first,policy in zip(r['evidence_slots'],r['evidence_policies']):
  first=int(first);low=539492;high=i['last_irreversible_block_num']
  assert first%12==0 and slot(block(low)['timestamp'])<first and slot(block(high)['timestamp'])>first+11
  while low<high:
   mid=(low+high)//2
   if slot(block(mid)['timestamp'])<first:low=mid+1
   else:high=mid
  a=block(low);b=block(low-1)
  assert slot(b['timestamp'])<first and slot(a['timestamp'])>first+11 and a['previous']==b['id']
  windows.append({'start_slot':first,'end_slot':first+11,'start_utc':stamp(first),'policy_version':policy,'produced_blocks':0,'before':{k:b[k]for k in['block_num','id','timestamp','producer']},'after':{k:a[k]for k in['block_num','id','timestamp','producer']}})
 ban=block(r['ban_final_block']);bond=bonds[name]
 assert int(bond['forfeited'])==100000000 and int(bond['balance'])==0
 result.append({'owner':name,'candidate':r,'bond':bond,'ban_checkpoint_timestamp':ban['timestamp'],'windows':windows})
 save('CANONICAL_NEW_BAN_PROGRESS.json',{'utc':now(),'completed':result})
x={'status':'PASS_TWELVE_CANONICAL_ZERO_WINDOWS','utc':now(),'chain_id':CHAIN,'observed_LIB':i['last_irreversible_block_num'],'nodes':result,'definitive_chain_trigger':'Three finalized personal12slot windows with zero blocks; native permanent ban and10000AIC forfeiture each','infrastructure_observation':'Shared4vCPU8GiB host had load~70,56GiBswap nearlyfull and CPU/IO pressure. Sydney service failed08:15UTC before all three windows. Other three processes remainedrunning, so permanent processdeath doesnotexplain their bans. Per-window OS scheduler/heap rootcause not fully established; no claim that all four were killed by OOM.'}
save('CANONICAL_NEW_BAN_AUDIT.json',x);print(json.dumps(x,ensure_ascii=False))
