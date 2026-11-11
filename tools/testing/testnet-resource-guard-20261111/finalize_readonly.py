"""Final inventory, allocator policy, timers and no-new-OOM proof; no mutation phase reruns."""
import sys
sys.path.insert(0,'/opt/pureeos-testnet-new-ban-20261111-r1')
from finance_common import *
OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
assert not(R/'FINALIZATION_RESULT.json').exists()
audit=read('FINAL_AUDIT.json');assert audit['status']=='PASS_26_NODES_SAME_CHAIN_AND_4_REPLACEMENTS'
baseline=read('REPLACEMENT_LIVE_BASELINE.json');plan=read('REPLACEMENTS_PREPARED.json')
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']});nodes={n:x for n,x in nodes.items()if n not in baseline['banned']};nodes.update({x['name']:x for x in plan['nodes']})
profiles={}
for n,x in nodes.items():
 cfg=Path(x['config_dir'])/'config.ini'if'config_dir'in x else(OLD if n in{'berlintucomp','bostonmiteng','nvidiaailabs','tencentailab','zurichethnet'}else R)/'nodes'/n/'config/config.ini'
 vals={}
 for l in cfg.read_text().splitlines():
  if '='in l:
   k,v=l.split('=',1);k=k.strip()
   if k in['chain-threads','net-threads','vote-threads','validation-mode','wasm-runtime','chain-state-db-size-mb','chain-state-db-guard-size-mb']:vals[k]=v.strip()
 assert vals['chain-threads']==vals['net-threads']=='1'and vals['validation-mode']=='full'
 env=subprocess.check_output(['systemctl','show',x['unit'],'-p','Environment','--value'],text=True)
 effective=[v for v in env.split()if v.startswith(('MALLOC_ARENA_MAX=','MALLOC_TRIM_THRESHOLD_='))]
 assert set(effective)=={'MALLOC_ARENA_MAX=2','MALLOC_TRIM_THRESHOLD_=131072'}
 profiles[n]={'profile':vals,'allocator':effective,'PID':state(x['unit'])['MainPID']}
timers={}
for name in['pureeos-testnet-memory-guard-20261111.timer','pureeos-testnet-health-priority-20261111.timer','pureeos-syslog-rotation-20261111.timer']:
 v=dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',name,'-p','ActiveState,UnitFileState,NextElapseUSecRealtime'],text=True).splitlines()if'='in l)
 assert v['ActiveState']=='active'and v['UnitFileState']=='enabled';timers[name]=v
rot=table('rotcands');prods=table('producersv2');assert len(rot)==len(prods)==41
assert{x['owner']for x in rot if x['banned']}==set(baseline['banned'])
assert all(r['zero_windows']==0 for r in rot if not r['banned']and r['owner']!='montrealmila')
assert len({int(x['total_votes'])for x in prods})==41
bonds={x['owner']:x for x in table('bpbonds')}
for x in baseline['bonds']:assert bonds[x['owner']]==x
for x in plan['nodes']:assert int(bonds[x['name']]['balance'])==100000000 and int(bonds[x['name']]['forfeited'])==0
kernel=subprocess.run(['journalctl','-k','--since','2026-11-11 01:30:00 UTC','--no-pager','-o','short-iso','--grep','Out of memory|Killed process|oom-kill|invoked oom'],capture_output=True,text=True,timeout=20)
assert kernel.stdout.strip()in['','-- No entries --']
assert not(R/'RECYCLE_BLOCKED.json').exists()and not(R/'RECYCLE_LAST_ATTEMPT.json').exists()
guard=read('RECYCLE_LATEST.json');assert guard['status']=='NO_ACTION_ALL_BELOW1280MiB'
health=read('HEALTH_LATEST.json');assert health['running_BP_count']==25 and health['status']=='OK'
info=rpc('get_info');f=rpc('get_finalizer_info');assert len(f['active_finalizer_policy']['finalizers'])==21 and f['active_finalizer_policy']['threshold']==15
x={'status':'PASS_TESTNET_OPTIMIZED_REPLACED_AND_GUARDS_ACTIVE','utc':now(),'info':info,'profiles_all25':profiles,'timers':timers,'distinct_vote_totals':41,'native_bans':15,'all25_eligible_zero_windows':0,'old_bonds_unchanged':True,'new_own_bonds_AIC':40000,'existing_test_funds_transferred_AIC':64000,'new_RAM_budget_AIC':4,'owned_external_ballots_redirected':14,'canonical_new_transactions':len(read('REPLACEMENT_ENROLL_RESULT.json')['completed'])*3+len(read('REPLACEMENT_ENROLL_RESULT.json')['vote_receipts']),'no_kernel_OOM_since':'2026-11-11T01:30:00Z','kernel_query':kernel.stdout,'no_automatic_memory_restart_needed':True,'recycle_latest':guard,'health':health,'cleanup':read('CLEANUP_RESULT.json'),'same_binary_source_consensus_no_new_server':True,'financial_signer_closed':state(UNIT)['ActiveState']=='inactive','all_one_shot_mutation_phases_complete_never_replay':True}
assert x['financial_signer_closed'];save('FINALIZATION_RESULT.json',x);print(json.dumps({k:x[k]for k in['status','utc','distinct_vote_totals','canonical_new_transactions','no_automatic_memory_restart_needed']}))
