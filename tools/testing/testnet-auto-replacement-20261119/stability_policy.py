"""Pure guarded selection. No RPC, process, file or transaction effects."""
MIB=1024**2;GIB=1024**3
LIMIT=768*MIB;EMERGENCY_LIMIT=512*MIB;COOLDOWN=180

def distance(name,active,slot,union):
 if name not in union:return 10000
 if name not in active:return 0
 turn=(slot//12)%len(active)
 return (((active.index(name)-turn)%len(active))*12-slot%12)/2

def select(s):
 if s['disk_free']<20*GIB:return None,'DISK_RESERVE'
 if s['mem_available']<512*MIB or s['swap_free']<2*GIB:return None,'INSUFFICIENT_RESTART_HEADROOM'
 if s['head_age']>5 or s['lib_age']>20:return None,'STALE_CHAIN'
 if s['policy_count']!=21 or s['threshold']!=15:return None,'WAIT_FULL_NATIVE21_15'
 if len(s['recent'])<19:return None,'INSUFFICIENT_FRESH_QUORUM'
 if not s['all_running']:return None,'MISSING_NONBANNED_BP'
 if s['elapsed']<COOLDOWN:return None,'COOLDOWN'
 limit=EMERGENCY_LIMIT if s['swap_free']<12*GIB else LIMIT
 choices=[]
 for n,v in s['nodes'].items():
  # Shared writer/relay/genesis source needs a reviewed role handoff before
  # planned recycling; ordinary BP recycling must never interrupt that role.
  if n=='gptgenesisbp':continue
  if v['banned']or not v['safety_exists']or v['bytes']<=limit:continue
  margin=110 if v['strikes']==2 else 90
  if v['strikes']not in [0,1,2]or v['distance']<margin or len(set(s['recent'])-{n})<18:continue
  choices.append(n)
 if not choices:
  shared=s['nodes'].get('gptgenesisbp')
  if shared and not shared['banned']and shared['bytes']>limit:return None,'SHARED_SOURCE_REQUIRES_REVIEW'
  return None,'NO_SAFE_OVERSIZED_TARGET'
 return max(choices,key=lambda n:(s['nodes'][n]['standby'],s['nodes'][n]['bytes'])),'QUALIFIED_ONE'

def reconciled(s):
 return(s['new_pid']not in ['0',s['old_pid']]and s['other_pids_unchanged']and s['anchor_matches']and s['target_fresh']and s['other_recent']>=18 and s['safety_exists']and s['config_unchanged']and not s['banned'])
