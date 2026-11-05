"""Reconcile the fifteen native zero-production windows with canonical slot gaps."""
from common import *
import traceback

def slot(ts):
    d=datetime.datetime.fromisoformat(ts.rstrip('Z')).replace(tzinfo=datetime.timezone.utc)-datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)
    return (d.days*86400+d.seconds)*2+d.microseconds//500000

try:
    assert not (R/'ZERO_WINDOW_CANONICAL_AUDIT.json').exists()
    bans=read('FIVE_NATIVE_BANS_IRREVERSIBLE.json');assert bans['status']=='PASS'
    stopped=read('STOP_RECEIPT.json');pre=read('PRE_STOP.json');plan=read('PLAN.json')
    assert all(not r['zero_windows'] for r in pre['snapshot']['candidates'] if r['owner'] in plan['fault_targets'])
    start_slot=slot(stopped['started_UTC']);end=rpc('get_info');assert end['chain_id']==CHAIN
    cache={}
    def block(n):
        if n not in cache:cache[n]=rpc('get_block',{'block_num_or_id':n},port=8890)
        return cache[n]
    low=int(plan['start_LIB']);high=min(end['last_irreversible_block_num'],rpc('get_info',port=8890)['last_irreversible_block_num'])
    evidence=[]
    for decision in bans['decisions']:
        row=decision['native_candidate'];windows=[]
        for first,policy in zip(row['evidence_slots'],row['evidence_policies']):
            first=int(first);assert first%12==0 and first>=start_slot
            assert slot(block(low)['timestamp'])<first and slot(block(high)['timestamp'])>first+11
            left=low;right=high
            while left<right:
                mid=(left+right)//2
                if slot(block(mid)['timestamp'])<first:left=mid+1
                else:right=mid
            after=block(left);before=block(left-1)
            assert slot(before['timestamp'])<first and slot(after['timestamp'])>first+11
            assert before['id']==after['previous']
            windows.append({'start_slot':first,'end_slot':first+11,'native_proposer_policy_version':int(policy),'canonical_produced_blocks_in_window':0,'before':{k:before[k] for k in ['block_num','id','timestamp','producer','schedule_version']},'after':{k:after[k] for k in ['block_num','id','timestamp','producer','schedule_version']}})
        assert len(windows)==3
        evidence.append({'producer':row['owner'],'windows':windows,'native_ban_final_block':row['ban_final_block'],'native_bond_record_block':decision['native_bond']['ban_record_block']})
    assert len(evidence)==5
    save('ZERO_WINDOW_CANONICAL_AUDIT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'observed_LIB':high,'stop_start_slot':start_slot,'native_zero_windows':15,'producers':evidence,'scope':'Native rotation candidate evidence is authoritative for personal assignment and policy version. Binary searches of irreversible canonical block timestamps prove each entire12-slot window contains zero produced blocks; adjacent block IDs link across the gap. All15 windows follow the real owned-node stop, with zero prior miss streaks. No timing/state edits.'})
    print('FIFTEEN_NATIVE_ZERO_WINDOWS_CANONICAL_GAPS_PASS',flush=True)
except BaseException:
    save('ZERO_WINDOW_CANONICAL_AUDIT_FAILURE.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
