"""Apply the user's immediate maintenance request to the sixth owned BP once."""
from common import *
import traceback

try:
    plan=read('PLAN.json');assert plan['maintenance_mode']=='immediate-owner-request'
    name=plan['maintenance_target'];assert name=='montrealmila'
    node=next(n for n in plan['nodes']if n['name']==name)
    assert rpc('get_info')['chain_id']==CHAIN;original_pids()
    assert state(node['unit'])['ActiveState']=='active'
    assert not(R/'MAINTENANCE_REQUEST.json').exists(),'Reconcile existing request; never submit it again'
    assert not any(x['owner']==name and x['state']in[1,2]for x in table('bpmaint'))
    proofs=read('DUTY_PROOFS.json')
    with Wallet()as wallet:
        wallet.import_key(read('private/keys.json')[name]['account'])
        a=sysact('setmaint',name,{'producer':name,'maintenance':True})
        a['authorization'].insert(0,{'actor':BANK,'permission':'active'})
        receipt=wallet.send('maintain-'+name,[a])
    save('MAINTENANCE_REQUEST.json',{'status':'PASS','UTC':now(),'producer':name,'canonical_receipt':receipt,'maintenance_mode':'immediate-owner-request','user_instruction':'第六个直接挂维护吧','completed_turns_before_request':proofs['complete_personal_production_turns'].get(name,0),'production_prerequisite_waived_by_user':True,'scope':'Owned producer signs its own native setmaint(true). Existing bank provides additional native CPU/NET authorization; no administrator override, key/vote/bond mutation, or duty-rule change.'})
    deadline=time.monotonic()+120
    while True:
        i=rpc('get_info');m=next((x for x in table('bpmaint')if x['owner']==name),None)
        p=rpc('get_producer_schedule');f=rpc('get_finalizer_info')
        members={x['producer_name']for x in p['active']['producers']}|{x['description']for x in f['active_finalizer_policy']['finalizers']}
        members|={x['producer_name']for x in(p.get('pending')or{}).get('producers',[])}|{x['description']for x in(f.get('pending_finalizer_policy')or{}).get('finalizers',[])}
        assert state(node['unit'])['ActiveState']=='active';original_pids()
        if m and m['state']==2 and 0<int(m['effective_block'])<=i['last_irreversible_block_num']and name not in members:
            bond=next(x for x in table('bpbonds')if x['owner']==name);assert int(bond['balance'])==100000000 and not int(bond['forfeited'])
            anchor=rpc('get_block',{'block_num_or_id':int(m['effective_block'])})
            save('MAINTENANCE_EFFECTIVE.json',{'status':'PASS','UTC':now(),'producer':name,'maintenance_mode':'immediate-owner-request','native_state':m,'effective_block_id':anchor['id'],'observed_info':i,'actual_proposer_schedule':p,'actual_finalizers':f,'process_kept_online':state(node['unit']),'bond':bond,'original25_PIDs_unchanged':original_pids(),'scope':'Native state2, irreversible effective checkpoint, absent active/pending proposer/finalizer duties; node remains running and its10000AIC bond stays locked. No requirement to produce first under the latest user instruction.'})
            print('SIXTH_NODE_NATIVE_MAINTENANCE_EFFECTIVE',json.dumps({'producer':name,'native_state':m,'transaction':receipt,'observed_LIB':i['last_irreversible_block_num']}),flush=True)
            break
        if time.monotonic()>=deadline:
            save('MAINTENANCE_HANDOVER_PENDING.json',{'status':'PENDING','UTC':now(),'producer':name,'native_state':m,'canonical_receipt':receipt,'scope':'Request is irreversible; later campaign observes native handover. Never resend the request.'})
            print('IRREVERSIBLE_REQUEST_NATIVE_HANDOVER_PENDING',flush=True);break
        time.sleep(2)
except BaseException:
    save('MAINTAIN_NOW_FAILURE.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
