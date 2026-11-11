"""Bounded final same-chain26node audit; no signing, publishing, node restarts or data changes."""
import sys
sys.path.insert(0,'/opt/pureeos-testnet-new-ban-20261111-r1')
from finance_common import *
OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
assert not(R/'FINAL_AUDIT_R2_STARTED.json').exists(),'Never replay this read-only continuation'
assert not(R/'FINAL_AUDIT.json').exists()
enroll=read('REPLACEMENT_ENROLL_RESULT.json');assert enroll['status']=='PASS_4_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS'
assert read('CANONICAL_NEW_BAN_AUDIT.json')['status']=='PASS_TWELVE_CANONICAL_ZERO_WINDOWS'
baseline=read('REPLACEMENT_LIVE_BASELINE.json');plan=read('REPLACEMENTS_PREPARED.json');banned=set(baseline['banned'])
def age(s):return(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(s.rstrip('Z')+'+00:00')).total_seconds()
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']});nodes={n:x for n,x in nodes.items()if n not in banned}
nodes.update({x['name']:x for x in plan['nodes']});nodes['history']={'unit':'pureeos-devnet-history-20261105.service','rpc_port':8890}
assert len(nodes)==26
start=read('FINAL_AUDIT_STARTED.json')['info'];samples=read('FINAL_SOAK_PROGRESS.json')['samples'];assert len(samples)==30
save('FINAL_AUDIT_R2_STARTED.json',{'utc':now(),'reuse_completed150sec_soak':True,'new4_native_checkpoint_history_boundary_recorded_instead_of_full_genesis_replay_claim':True})
end=rpc('get_info');assert end['last_irreversible_block_num']>start['last_irreversible_block_num']+120
allnodes={}
for n,x in nodes.items():
 s=state(x['unit']);assert int(s['MainPID'])>0 and s['ActiveState']=='active'
 i=rpc('get_info',port=x['rpc_port']);assert i['chain_id']==CHAIN and age(i['head_block_time'])<15 and age(i['last_irreversible_block_time'])<90
 p=Path('/proc')/s['MainPID'];assert str(V/'libexec/nodeos')in(p/'maps').read_text()
 m={l.split(':')[0]:int(l.split()[1])*1024 for l in(p/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:'))}
 allnodes[n]={'unit':x['unit'],'state':s,'info':i,'memory':m}
assert len({x['state']['MainPID']for x in allnodes.values()})==26
physical=[]
for p in Path('/proc').iterdir():
 if p.name.isdigit():
  try:
   if str(V/'libexec/nodeos')in(p/'maps').read_text():physical.append(int(p.name))
  except OSError:pass
assert set(physical)=={int(x['state']['MainPID'])for x in allnodes.values()}
common=min(x['info']['last_irreversible_block_num']for x in allnodes.values());ids={n:rpc('get_block',{'block_num_or_id':common},port=x['rpc_port'])['id']for n,x in nodes.items()};assert len(set(ids.values()))==1
assert rpc('get_block',{'block_num_or_id':532319})['id']=='00081f5fa4d3716fca57d7516e19e0c32e50bcb6be7e016c1d838b5856b5866d'
genesis=rpc('get_block',{'block_num_or_id':1})['id'];assert rpc('get_block',{'block_num_or_id':1},port=8890)['id']==genesis
new_genesis={}
for x in plan['nodes']:
 try:
  bid=rpc('get_block',{'block_num_or_id':1},port=x['rpc_port'])['id'];assert bid==genesis;new_genesis[x['name']]={'block1_id':bid}
 except RuntimeError as error:
  assert 'unknown_block_exception'in str(error)
  new_genesis[x['name']]={'block1':'NOT_LOCAL_NATIVE_CHECKPOINT_BOOTSTRAP','earliest_available_block_num':allnodes[x['name']]['info']['earliest_available_block_num'],'scope':'Full validation from same-chain irreversible checkpoint; complete genesis history remains on founder and history reader.'}

rot=table('rotcands');assert len(rot)==41 and{x['owner']for x in rot if x['banned']}==banned
maint=table('bpmaint');assert[x['owner']for x in maint if x['state']==2]==['montrealmila']
assert all(x['zero_windows']==0 for x in rot if not x['banned']and x['owner']!='montrealmila')
assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==BINARY
for c,sym in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]:assert rpc('get_currency_stats',{'code':c,'symbol':sym})==baseline['token_stats'][c]
for a,pin in baseline['code_ABI_pins'].items():assert all(rpc('get_raw_abi',{'account_name':a})[k]==v for k,v in pin.items())
assert table('relays','eosio.key','eosio.key')==baseline['relays']
f=rpc('get_finalizer_info');assert sum(v['is_vote_strong']and age(v['voted_for_block_timestamp'])<10 for v in f['last_tracked_votes'])>=18
assert state(UNIT)['ActiveState']=='inactive'
def public(path,body=None):
 q=urllib.request.Request('https://aic-test.matrixai.one'+path,None if body is None else json.dumps(body).encode(),{'Content-Type':'application/json','User-Agent':'PureEOS-Wallet/0.1.0-test.28 Android'})
 with urllib.request.urlopen(q,timeout=20)as f:return json.load(f)
pub={'reader':public('/api/v1/chain/get_info',{}),'writer':public('/rpc/v1/chain/get_info',{})}
for v in pub.values():assert v['chain_id']==CHAIN and age(v['head_block_time'])<15 and age(v['last_irreversible_block_time'])<90
summary=public('/history/v2/state/get_producer_summary');assert summary['available']
assert[summary[k]for k in['registered_bp_count','eligible_bp_count','active_producer_count','permanent_blacklist_count','maintenance_excluded_count']]==[41,25,21,15,1]
hist=public('/history/v2/health');assert all(v['status']=='OK'for v in hist['health'])and age(hist['last_indexed_block_time'])<30
timers={}
for u in['pureeos-syslog-rotation-20261111.timer','pureeos-testnet-health-priority-20261111.timer']:
 v=dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','ActiveState,UnitFileState,NextElapseUSecRealtime'],text=True).splitlines()if'='in l)
 assert v['ActiveState']=='active'and v['UnitFileState']=='enabled';timers[u]=v
rotation=state('pureeos-syslog-rotation-20261111.service');assert rotation['ActiveState']=='inactive','Initial gzip stillrunning; finish before final disk claim'
assert subprocess.check_output(['systemctl','show','pureeos-syslog-rotation-20261111.service','-p','Result','--value'],text=True).strip()=='success'
mem={l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapTotal:','SwapFree:'))}
assert mem['MemAvailable']>700*1024**2 and mem['SwapFree']>12*1024**3
s=os.statvfs('/');assert s.f_bavail*s.f_frsize>20*1024**3
result={'status':'PASS_26_NODES_SAME_CHAIN_AND_4_REPLACEMENTS','utc':now(),'start':start,'end':end,'soak_samples':samples,'all26_nodes':allnodes,'physical_node_PIDs':physical,'shared_LIB':common,'shared_LIB_id':next(iter(ids.values())),'new4_history_availability':new_genesis,'founder_and_history_retained_genesis_id':genesis,'old_anchor532319_retained':True,'registered':41,'eligible':25,'banned':sorted(banned),'maintenance':maint,'native_SHA256':BINARY,'native_source_binary_consensus_and_system_token_contracts_unchanged':True,'recent_strong_votes':sum(v['is_vote_strong']and age(v['voted_for_block_timestamp'])<10 for v in f['last_tracked_votes']),'native_finalizer_policy':f['active_finalizer_policy'],'public_RPC':pub,'public_summary':summary,'public_history_health':hist,'health':read('HEALTH_LATEST.json'),'memory':mem,'disk_available_bytes':s.f_bavail*s.f_frsize,'timers':timers,'logrotate_initial_complete':True,'log_files':[{'path':str(p),'bytes':p.stat().st_size,'allocated':p.stat().st_blocks*512}for p in Path('/var/log').glob('syslog*')],'new_nodes_natural_FIFO_no_forced_schedule':True,'not_a_TPS_or_indefinite_uptime_claim':True}
save('FINAL_AUDIT.json',result);print(json.dumps({'status':result['status'],'utc':result['utc'],'memory':mem,'disk_available_bytes':result['disk_available_bytes']}))
