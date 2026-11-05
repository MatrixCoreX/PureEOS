"""User-authorized stop of three new BPs and the two already-banned originals."""
from common import *
import traceback

SELECTED=['tokyoaistlab','pariscnrslab','londonturing']
BANNED=['tokyorikenai','singaporentu']
def node_unit(name):
 if name=='gptgenesisbp':return next(u for u in read('BASELINE.json')['node_pids']if 'founder'in u)
 return 'pureeos-bp-demo-'+name+'-20261105.service'if name in SELECTED+['genevacernai','sydneyunswai','montrealmila']else'pureeos-devnet-bp-'+name+'-20261105.service'
try:
 assert not(R/'USER_THREE_STOP_RECEIPT_20261106.json').exists(),'Reconcile existing stop; do not repeat'
 assert state('pureeos-bp-demo-six-campaign-r5-20261105.service')['ActiveState']!='active','Superseded five-target controller must not act'
 start=rpc('get_info');assert start['chain_id']==CHAIN
 cands={x['owner']:x for x in table('rotcands')}
 assert all(cands[n]['banned']and 0<int(cands[n]['ban_final_block'])<=start['last_irreversible_block_num']for n in BANNED)
 f=rpc('get_finalizer_info');votes={v['description']:v for v in f.get('last_tracked_votes',[])};policies=[f['active_finalizer_policy']]
 if f.get('pending_finalizer_policy'):policies.append(f['pending_finalizer_policy'])
 guards=[]
 for policy in policies:
  assert len(policy['finalizers'])==21 and policy['threshold']==15
  remaining={v['description']for v in policy['finalizers']}-set(SELECTED)
  healthy=[n for n in remaining if state(node_unit(n))['ActiveState']=='active'and n in votes and votes[n]['is_vote_strong']and votes[n]['voted_for_block_num']>=start['last_irreversible_block_num']-24]
  assert len(healthy)>=16,'Wait for at least16 recent strong survivors; do not reduce native quorum'
  guards.append({'generation':policy['generation'],'threshold':15,'remaining_recent_strong_online':sorted(healthy)})
 units=[node_unit(n)for n in BANNED+SELECTED];assert len(set(units))==5
 processes={u:state(u)for u in units};save('USER_THREE_STOP_BEFORE_20261106.json',{'UTC':now(),'user_instructions':['已经有2个节点进入黑名单了,你直接帮我另外3个停机','然后清除这5个节点所有区块数据 释放服务器'],'selected_new_three':SELECTED,'already_banned_two':BANNED,'info':start,'quorum_guards':guards,'processes':processes,'memory':subprocess.check_output(['free','-m'],text=True)})
 subprocess.run(['systemctl','stop',*units],check=True,timeout=180)
 subprocess.run(['systemctl','disable',*units],check=True,capture_output=True)
 stopped={u:state(u)for u in units};assert all(x['ActiveState']=='inactive'and x['MainPID']=='0'and x['UnitFileState']=='disabled'for x in stopped.values())
 save('USER_THREE_STOP_RECEIPT_20261106.json',{'status':'STOPPED_AWAITING_FINALITY_CHECK','UTC':now(),'selected_new_three':SELECTED,'already_banned_two':BANNED,'only_five_units':units,'processes':stopped,'quorum_guards':guards,'before_info':start,'scope':'Latest explicit user instruction supersedes the earlier five-new-node wait gate. Only these five processes were stopped/disabled. Data cleanup is a separate checked step.'})
 deadline=time.monotonic()+90
 while True:
  end=rpc('get_info');assert end['chain_id']==CHAIN
  if end['last_irreversible_block_num']>start['last_irreversible_block_num']:break
  assert time.monotonic()<deadline,'No irreversible progress; keep data intact for reconciliation';time.sleep(2)
 receipt=read('USER_THREE_STOP_RECEIPT_20261106.json');receipt.update(status='PASS',after_info=end,irreversible_progress_after_stop=True);save('USER_THREE_STOP_RECEIPT_20261106.json',receipt)
 print('FIVE_SCOPED_UNITS_STOPPED_DISABLED_FINALITY_PROGRESS',flush=True)
except BaseException:
 save('USER_THREE_STOP_FAILURE_20261106.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
